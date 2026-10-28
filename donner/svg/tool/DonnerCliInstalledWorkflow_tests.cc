/// @file

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/FileUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/base/tests/TestTempDir.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/GifTestDecoder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/svg_suite/ImageCompare.h"

namespace donner::svg {
namespace {

namespace fs = std::filesystem;
using testing::AllOf;
using testing::Eq;
using testing::Ge;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using testing::Optional;

constexpr std::string_view kMouseEnabled = "\x1b[?1000h\x1b[?1006h";
constexpr size_t kMaximumOutputBytes = 2 * 1024 * 1024;

class Descriptor {
public:
  explicit Descriptor(int value = -1) : value_(value) {}
  ~Descriptor() { reset(); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const { return value_; }
  void reset(int value = -1) {
    if (value_ >= 0) {
      close(value_);
    }
    value_ = value;
  }

private:
  int value_;
};

struct ProcessResult {
  int exitCode = -1;
  bool quitSent = false;
  std::string output;
  std::string error;
};

bool DrainOutput(int descriptor, ProcessResult& result) {
  std::array<char, 4096> bytes{};
  for (int reads = 0; reads < 32; ++reads) {
    const ssize_t count = read(descriptor, bytes.data(), bytes.size());
    if (count > 0) {
      if (static_cast<size_t>(count) > kMaximumOutputBytes - result.output.size()) {
        result.error = "CLI output exceeded 2 MiB";
        return false;
      }
      result.output.append(bytes.data(), static_cast<size_t>(count));
    } else if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else if (errno != EINTR) {
      result.error = "Reading CLI output failed: " + std::string(std::strerror(errno));
      return false;
    }
  }
  return true;
}

struct ProcessDescriptors {
  Descriptor reader;
  Descriptor writer;
  Descriptor input;
  Descriptor master;
};

struct PreparedInvocation {
  PreparedInvocation() = default;
  PreparedInvocation(const PreparedInvocation&) = delete;
  PreparedInvocation& operator=(const PreparedInvocation&) = delete;
  PreparedInvocation(PreparedInvocation&&) = delete;
  PreparedInvocation& operator=(PreparedInvocation&&) = delete;

  std::vector<std::string> storage;
  std::vector<char*> argv;
  std::vector<std::string> environment;
  std::vector<char*> envp;
  std::string directory;
};

struct ChildState {
  int status = 0;
  bool owned = true;
  bool reaped = false;
};

bool PrepareTerminal(ProcessDescriptors& io, ProcessResult& result) {
  io.master.reset(posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
  if (io.master.get() < 0 || grantpt(io.master.get()) != 0 || unlockpt(io.master.get()) != 0) {
    result.error =
        "Creating required CLI pseudo-terminal failed: " + std::string(std::strerror(errno));
    return false;
  }
  const char* slave = ptsname(io.master.get());
  io.input.reset(slave ? open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1);
  const winsize size{24, 80, 0, 0};
  if (io.input.get() < 0 || ioctl(io.input.get(), TIOCSWINSZ, &size) != 0) {
    result.error = "Opening required CLI pseudo-terminal slave failed";
    return false;
  }
  return true;
}

bool PrepareDescriptors(ProcessDescriptors& io, bool interactive, ProcessResult& result) {
  int pipeEnds[2];
  if (pipe(pipeEnds) != 0) {
    result.error = "Creating CLI output pipe failed: " + std::string(std::strerror(errno));
    return false;
  }
  io.reader.reset(pipeEnds[0]);
  io.writer.reset(pipeEnds[1]);
  io.input.reset(open("/dev/null", O_RDONLY | O_CLOEXEC));
  if (fcntl(io.reader.get(), F_SETFL, O_NONBLOCK) != 0 || io.input.get() < 0) {
    result.error = "Configuring CLI descriptors failed";
    return false;
  }
  return !interactive || PrepareTerminal(io, result);
}

void PrepareInvocation(PreparedInvocation& invocation, const fs::path& executable,
                       const fs::path& cwd, const fs::path& home,
                       const std::vector<std::string>& arguments) {
  invocation.storage.emplace_back(executable.string());
  invocation.storage.insert(invocation.storage.end(), arguments.begin(), arguments.end());
  for (std::string& argument : invocation.storage) {
    invocation.argv.push_back(argument.data());
  }
  invocation.argv.push_back(nullptr);
  invocation.environment = {"PATH=/usr/bin:/bin", "LC_ALL=C", "TERM=xterm-256color",
                            "COLORTERM=truecolor", "HOME=" + home.string()};
  for (std::string& value : invocation.environment) {
    invocation.envp.push_back(value.data());
  }
  invocation.envp.push_back(nullptr);
  invocation.directory = cwd.string();
}

pid_t StartChild(const PreparedInvocation& invocation, const ProcessDescriptors& io,
                 ProcessResult& result) {
  const pid_t child = fork();
  if (child < 0) {
    result.error = "Starting CLI failed: " + std::string(std::strerror(errno));
    return child;
  }
  if (child == 0) {
    close(io.reader.get());
    if (dup2(io.writer.get(), STDOUT_FILENO) < 0 || dup2(io.writer.get(), STDERR_FILENO) < 0 ||
        dup2(io.input.get(), STDIN_FILENO) < 0 || chdir(invocation.directory.c_str()) != 0) {
      _exit(126);
    }
    close(io.writer.get());
    close(io.input.get());
    execve(invocation.argv[0], invocation.argv.data(), invocation.envp.data());
    constexpr char message[] = "Cannot execute staged donner-cli\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
  }
  return child;
}

bool ObserveExit(pid_t child, int reader, ChildState& state, ProcessResult& result) {
  const pid_t observed = waitpid(child, &state.status, WNOHANG);
  if (observed == child) {
    state.owned = false;
    state.reaped = true;
    (void)DrainOutput(reader, result);
    return true;
  }
  if (observed < 0 && errno != EINTR) {
    state.owned = false;
    result.error = "Waiting for CLI failed: " + std::string(std::strerror(errno));
    return true;
  }
  return false;
}

bool SendInteractiveQuit(int master, bool interactive, ProcessResult& result) {
  if (interactive && !result.quitSent && result.output.find(kMouseEnabled) != std::string::npos) {
    const ssize_t count = write(master, "q", 1);
    if (count == 1) {
      result.quitSent = true;
    } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      result.error = "Sending interactive quit failed";
      return false;
    }
  }
  return true;
}

void PollOutput(int reader, ProcessResult& result) {
  pollfd descriptor{reader, POLLIN, 0};
  if (poll(&descriptor, 1, 10) < 0 && errno != EINTR) {
    result.error = "Polling CLI output failed";
  }
}

void WaitForExit(pid_t child, const ProcessDescriptors& io, bool interactive, ChildState& state,
                 ProcessResult& result) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (state.owned && result.error.empty()) {
    if (!DrainOutput(io.reader.get(), result)) {
      break;
    }
    if (ObserveExit(child, io.reader.get(), state, result)) {
      break;
    }
    if (!SendInteractiveQuit(io.master.get(), interactive, result)) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.error = "CLI exceeded its 5-second invocation deadline";
      break;
    }
    PollOutput(io.reader.get(), result);
  }
}

void ReapOwnedChild(pid_t child, ChildState& state, ProcessResult& result) {
  (void)kill(child, SIGKILL);
  // Reaping is mandatory even if the OS cannot finish termination within the deadline.
  pid_t waited;
  do {
    waited = waitpid(child, &state.status, 0);
  } while (waited < 0 && errno == EINTR);
  state.reaped = waited == child;
  if (!state.reaped) {
    result.error += "; cannot reap CLI: " + std::string(std::strerror(errno));
  }
}

void SetExitCode(const ChildState& state, ProcessResult& result) {
  if (state.reaped && WIFEXITED(state.status)) {
    result.exitCode = WEXITSTATUS(state.status);
  } else if (state.reaped && WIFSIGNALED(state.status)) {
    result.exitCode = 128 + WTERMSIG(state.status);
  }
}

ProcessResult RunInstalled(const fs::path& executable, const fs::path& cwd, const fs::path& home,
                           const std::vector<std::string>& arguments, bool interactive = false) {
  ProcessResult result;
  ProcessDescriptors io;
  if (!PrepareDescriptors(io, interactive, result)) {
    return result;
  }
  PreparedInvocation invocation;
  PrepareInvocation(invocation, executable, cwd, home, arguments);
  const pid_t child = StartChild(invocation, io, result);
  if (child < 0) {
    return result;
  }
  io.writer.reset();
  io.input.reset();
  ChildState state;
  WaitForExit(child, io, interactive, state, result);
  if (state.owned) {
    ReapOwnedChild(child, state, result);
  }
  SetExitCode(state, result);
  return result;
}

std::string ReadFile(const fs::path& path) {
  FileReadResult result = ReadFileBounded(path, kMaximumOutputBytes);
  if (const auto* error = std::get_if<FileReadError>(&result)) {
    ADD_FAILURE() << path << ": " << FileReadErrorMessage(*error);
    return {};
  }
  return std::get<std::string>(std::move(result));
}

bool CopyTree(const fs::path& source, const fs::path& destination, std::error_code& error) {
  fs::create_directories(destination, error);
  for (fs::recursive_directory_iterator entry(source, error), end; !error && entry != end;
       entry.increment(error)) {
    const fs::path target = destination / entry->path().lexically_relative(source);
    if (entry->is_directory(error)) {
      fs::create_directory(target, error);
    } else if (entry->is_regular_file(error)) {
      fs::copy_file(entry->path(), target, error);
      if (!error) {
        fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, error);
      }
    } else {
      error = std::make_error_code(std::errc::invalid_argument);
    }
  }
  return !error;
}

RendererBitmap FrameBitmap(const std::vector<uint8_t>& pixels, int width, int height) {
  return {Vector2i(width, height), pixels, static_cast<size_t>(width) * 4,
          AlphaType::Unpremultiplied};
}

class DonnerCliInstalledWorkflow : public testing::Test {
protected:
  void SetUp() override {
    std::string pattern = (TestTempDir() / "donner-cli-installed-XXXXXX").string();
    ASSERT_THAT(mkdtemp(pattern.data()), Not(testing::IsNull()));
    root_ = pattern;
    install_ = root_ / "install";
    cwd_ = root_ / "unrelated";
    home_ = root_ / "home";
    std::error_code error;
    for (const fs::path& directory : {install_, cwd_, home_}) {
      fs::create_directory(directory, error);
      ASSERT_THAT(error, Eq(std::error_code())) << directory;
    }
    const fs::path source = Runfiles::instance().Rlocation("donner/svg/tool/donner-cli");
    executable_ = install_ / "donner-cli";
    fs::copy_file(source, executable_, error);
    ASSERT_THAT(error, Eq(std::error_code())) << source;
    fs::permissions(executable_,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, error);
    ASSERT_THAT(error, Eq(std::error_code()));
    assetsSource_ = Runfiles::instance().Rlocation("donner/svg/tool/donner-assets");
    ASSERT_THAT(fs::is_directory(assetsSource_, error), Eq(true)) << assetsSource_ << ": " << error;
    input_ = writeSvg("input.svg", true);
  }

  void TearDown() override {
    if (!root_.empty()) {
      std::error_code error;
      fs::remove_all(root_, error);
      EXPECT_THAT(error, Eq(std::error_code())) << root_;
    }
  }

  fs::path writeSvg(std::string_view name, bool text) {
    const fs::path path = cwd_ / name;
    std::ofstream stream(path);
    stream << R"(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64">
<rect width="128" height="64" fill="white"/><rect width="8" height="8" fill="blue"/>)";
    if (text) {
      stream
          << R"(<text x="4" y="45" font-family="UninstalledFixtureFamily" font-size="32">Hi</text>)";
    }
    stream << "</svg>";
    EXPECT_THAT(stream.good(), Eq(true)) << path;
    return path;
  }

  void installAssets(const fs::path& destination = {}) {
    std::error_code error;
    EXPECT_THAT(CopyTree(assetsSource_,
                         destination.empty() ? install_ / "donner-assets" : destination, error),
                Eq(true))
        << error;
  }

  ProcessResult invoke(const std::vector<std::string>& arguments, bool interactive = false) {
    ProcessResult result = RunInstalled(executable_, cwd_, home_, arguments, interactive);
    EXPECT_THAT(result.error, IsEmpty()) << result.output;
    if (const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
      const fs::path log =
          fs::path(output) /
          (std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
           std::to_string(invocations_++) + ".log");
      std::ofstream stream(log);
      stream << "exit=" << result.exitCode << " quitSent=" << result.quitSent << "\n"
             << result.error << "\n"
             << result.output;
    }
    return result;
  }

  void keepArtifact(const fs::path& path) {
    if (const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
      std::error_code error;
      fs::copy_file(path, fs::path(output) / path.filename(), fs::copy_options::overwrite_existing,
                    error);
      EXPECT_THAT(error, Eq(std::error_code())) << path;
    }
  }

  void expectResourceRefusal() {
    const fs::path output = cwd_ / "refused.png";
    const ProcessResult result = invoke({input_.string(), "--output", output.string()});
    EXPECT_THAT(result.exitCode, Eq(2)) << result.output;
    EXPECT_THAT(result.output, HasSubstr("Failed to load packaged resources:"));
    EXPECT_THAT(fs::exists(output), Eq(false)) << output;
  }

  fs::path root_;
  fs::path install_;
  fs::path cwd_;
  fs::path home_;
  fs::path executable_;
  fs::path assetsSource_;
  fs::path input_;
  int invocations_ = 0;
};

TEST_F(DonnerCliInstalledWorkflow, HelpAndInvalidArgumentsDoNotRequireAssets) {
  const ProcessResult help = invoke({"--help"});
  EXPECT_THAT(help.exitCode, Eq(0)) << help.output;
  EXPECT_THAT(help.output, AllOf(HasSubstr("donner-cli"), HasSubstr("--interactive")));
  const ProcessResult invalid = invoke({"--bogus"});
  EXPECT_THAT(invalid.exitCode, Eq(1)) << invalid.output;
  EXPECT_THAT(invalid.output, Not(HasSubstr("Failed to load packaged resources:")));
}

TEST_F(DonnerCliInstalledWorkflow, RelocatedAssetsRenderFallbackGlyphsToPng) {
  installAssets();
  const fs::path shapes = writeSvg("shapes.svg", false);
  const fs::path shapesPng = cwd_ / "shapes.png";
  const fs::path textPng = cwd_ / "text.png";
  for (const auto& [source, destination] :
       {std::pair{shapes, shapesPng}, std::pair{input_, textPng}}) {
    const ProcessResult result = invoke({source.string(), "--output", destination.string()});
    ASSERT_THAT(result.exitCode, Eq(0)) << result.output;
    keepArtifact(destination);
  }
  const auto shapesImage = svg_suite::ReadPng(shapesPng);
  const auto textImage = svg_suite::ReadPng(textPng);
  ASSERT_THAT(shapesImage.error, IsEmpty());
  ASSERT_THAT(textImage.error, IsEmpty());
  ASSERT_THAT(shapesImage.image.has_value(), Eq(true));
  ASSERT_THAT(textImage.image.has_value(), Eq(true));
  const auto& expected = *shapesImage.image;
  const auto& actual = *textImage.image;
  EXPECT_THAT(actual.width, Eq(128));
  EXPECT_THAT(actual.height, Eq(64));
  const auto comparison = svg_suite::CompareImages(
      {expected.width, expected.height, expected.strideInPixels, expected.data},
      {actual.width, actual.height, actual.strideInPixels, actual.data});
  EXPECT_THAT(comparison.error, IsEmpty());
  EXPECT_THAT(comparison.mismatchedPixels, Optional(Ge(20)));
}

TEST_F(DonnerCliInstalledWorkflow, PreviewRendersWithoutWritingDefaultPng) {
  installAssets();
  const ProcessResult result = invoke({input_.string(), "--preview"});
  EXPECT_THAT(result.exitCode, Eq(0)) << result.output;
  EXPECT_THAT(result.output, AllOf(HasSubstr("Rendered size: 128x64"), HasSubstr("\x1b["),
                                   Not(HasSubstr("Saved PNG:"))));
  EXPECT_THAT(fs::exists(cwd_ / "output.png"), Eq(false));
}

TEST_F(DonnerCliInstalledWorkflow, GifContainsTheTwoExpectedAnimationFrames) {
  installAssets();
  const fs::path source = cwd_ / "animated.svg";
  std::ofstream(source) << R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10">
<rect width="20" height="10" fill="white"/><rect x="0" y="2" width="4" height="6" fill="blue">
<animate attributeName="x" from="0" to="16" dur="1s" fill="freeze"/></rect></svg>)";
  const fs::path output = cwd_ / "animated.gif";
  const ProcessResult result =
      invoke({source.string(), "--gif", output.string(), "--end", "1", "--fps", "2"});
  ASSERT_THAT(result.exitCode, Eq(0)) << result.output;
  keepArtifact(output);
  const std::string bytes = ReadFile(output);
  std::string error;
  const auto gif =
      DecodeGifForTesting({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, &error);
  ASSERT_THAT(error, IsEmpty());
  ASSERT_THAT(gif.has_value(), Eq(true));
  ASSERT_THAT(gif->frames, testing::SizeIs(2));
  ASSERT_THAT(gif->width, Eq(20));
  ASSERT_THAT(gif->height, Eq(10));
  EXPECT_THAT(gif->loopCount, Optional(0));
  for (size_t frame = 0; frame < gif->frames.size(); ++frame) {
    EXPECT_THAT(gif->frames[frame].delayCentiseconds, Eq(50));
    std::vector<uint8_t> expected(20 * 10 * 4, 255);
    for (int y = 2; y < 8; ++y) {
      for (int x = static_cast<int>(frame) * 8; x < static_cast<int>(frame) * 8 + 4; ++x) {
        const size_t offset = static_cast<size_t>((y * 20 + x) * 4);
        expected[offset] = 0;
        expected[offset + 1] = 0;
      }
    }
    editor::tests::CompareBitmapToBitmap(FrameBitmap(gif->frames[frame].rgba, 20, 10),
                                         FrameBitmap(expected, 20, 10),
                                         "installed_cli_gif_frame_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(DonnerCliInstalledWorkflow, InteractiveQuitWaitsForTheTerminalReadyMarker) {
  installAssets();
  const ProcessResult result = invoke({input_.string(), "--interactive"}, true);
  EXPECT_THAT(result.exitCode, Eq(0)) << result.output;
  EXPECT_THAT(result.quitSent, Eq(true)) << result.output;
  EXPECT_THAT(result.output,
              AllOf(HasSubstr(std::string(kMouseEnabled)), HasSubstr("\x1b[?1000l\x1b[?1006l"),
                    Not(HasSubstr("Interactive mode needs a TTY"))));
}

TEST_F(DonnerCliInstalledWorkflow, WorkingDirectoryAssetsCannotReplaceMissingCompanion) {
  installAssets(cwd_ / "donner-assets");
  expectResourceRefusal();
}

TEST_F(DonnerCliInstalledWorkflow, CorruptAssetIsRefused) {
  installAssets();
  const fs::path assets = install_ / "donner-assets" / "assets";
  std::error_code error;
  const fs::directory_iterator entry(assets, error);
  ASSERT_THAT(error, Eq(std::error_code()));
  ASSERT_THAT(entry, Not(Eq(fs::directory_iterator())));
  std::string bytes = ReadFile(entry->path());
  ASSERT_THAT(bytes, Not(IsEmpty()));
  bytes.front() ^= 1;
  std::ofstream(entry->path(), std::ios::binary | std::ios::trunc)
      .write(bytes.data(), bytes.size());
  expectResourceRefusal();
}

TEST_F(DonnerCliInstalledWorkflow, UnknownCompiledIdentityIsRefused) {
  installAssets();
  const fs::path manifest = install_ / "donner-assets" / "manifest.json";
  std::string content = ReadFile(manifest);
  constexpr std::string_view marker = "\"id\":\"k";
  const size_t identity = content.find(marker);
  ASSERT_THAT(identity, Not(Eq(std::string::npos)));
  const size_t character = identity + marker.size();
  ASSERT_THAT(character, testing::Lt(content.size()));
  ASSERT_THAT(content[character], Not(Eq('"')));
  content[character] = content[character] == 'x' ? 'y' : 'x';
  std::ofstream(manifest, std::ios::binary | std::ios::trunc).write(content.data(), content.size());
  expectResourceRefusal();
}

TEST_F(DonnerCliInstalledWorkflow, MalformedManifestIsRefused) {
  installAssets();
  std::ofstream(install_ / "donner-assets" / "manifest.json") << "{malformed";
  expectResourceRefusal();
}

TEST_F(DonnerCliInstalledWorkflow, MalformedNamedCompanionCannotUseValidGenericTree) {
  installAssets();
  const fs::path named = install_ / "donner-cli.assets";
  installAssets(named);
  std::ofstream(named / "manifest.json") << "{malformed";
  expectResourceRefusal();
}

}  // namespace
}  // namespace donner::svg
