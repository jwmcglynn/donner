/// @file
/// Child-process failure, deadline, and argv boundaries exercised with this test executable.

#include "tools/svg_suite/ProcessRenderer.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg_suite {
namespace {

std::filesystem::path executable;

// A descendant keeps the write end open until it exits. EOF proves termination without relying
// on process-list timing or on an orphan zombie being promptly reaped by the host.
class DescendantProbe {
public:
  bool open() { return pipe(descriptors_) == 0; }

  ~DescendantProbe() {
    if (descendant_ > 0 && !stopped_) {
      // A broken cleanup implementation must not leave the regression fixture running.
      kill(descendant_, SIGKILL);
      pollfd descriptor{descriptors_[0], POLLIN | POLLHUP, 0};
      poll(&descriptor, 1, 1000);
    }
    for (const int descriptor : descriptors_) {
      if (descriptor >= 0) {
        close(descriptor);
      }
    }
  }

  std::string mode(std::string_view outcome) const {
    return "descendant-" + std::string(outcome) + ":" + std::to_string(descriptors_[1]);
  }

  bool stopped() {
    close(descriptors_[1]);
    descriptors_[1] = -1;
    pollfd descriptor{descriptors_[0], POLLIN | POLLHUP, 0};
    if (poll(&descriptor, 1, 1000) <= 0 ||
        read(descriptors_[0], &descendant_, sizeof(descendant_)) !=
            static_cast<ssize_t>(sizeof(descendant_)) ||
        descendant_ <= 0) {
      return false;
    }
    if (poll(&descriptor, 1, 1000) <= 0) {
      return false;
    }
    char unexpected;
    stopped_ = read(descriptors_[0], &unexpected, 1) == 0;
    return stopped_;
  }

private:
  int descriptors_[2] = {-1, -1};
  pid_t descendant_ = -1;
  bool stopped_ = false;
};

class ProcessRendererTest : public testing::Test {
protected:
  void SetUp() override {
    const char* temporary = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary, nullptr) << "TEST_TMPDIR must name a test-owned work directory";
    std::string pattern = (std::filesystem::path(temporary) / "process-tests-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
    test_ = StaticCase{"fixture", root_, root_ / "input {root} {output} ; literal.svg",
                       root_ / "expected.png"};
  }

  ImageResult render(std::string mode,
                     std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    return MakeProcessRenderer(
        {executable, root_, {"--fixture", std::move(mode), "{output}", "{input}"}, timeout, true})(
        test_, 1, 1);
  }

  std::filesystem::path root_;
  StaticCase test_;
};

TEST_F(ProcessRendererTest, EmbeddedNulArgumentIsRejectedBeforeLaunch) {
  const auto result = render(std::string("success\0unrecorded", 18));
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("UTF-8"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "fixture/process/render.png"));
}

TEST_F(ProcessRendererTest, InvalidUtf8ArgumentIsRejectedBeforeLaunch) {
  const auto result = render(std::string("invalid-") + char(0xFF));
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("UTF-8"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "fixture/process"));
}

TEST_F(ProcessRendererTest, SuccessfulRenderPreservesLiteralArguments) {
  const auto result = render("success");
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(255, 0, 0, 255));
}

TEST_F(ProcessRendererTest, SuccessfulRendererDoesNotLeaveDescendantRunning) {
  DescendantProbe probe;
  ASSERT_TRUE(probe.open());
  const auto start = std::chrono::steady_clock::now();
  const auto result = render(probe.mode("success"));
  EXPECT_THAT(result.error, testing::IsEmpty());
  EXPECT_NE(result.image, std::nullopt);
  EXPECT_TRUE(probe.stopped()) << "successful renderer left a live descendant";
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST_F(ProcessRendererTest, FailedRendererDoesNotLeaveDescendantRunning) {
  DescendantProbe probe;
  ASSERT_TRUE(probe.open());
  const auto result = render(probe.mode("failure"));
  EXPECT_THAT(result.error, testing::HasSubstr("renderer exit 7"));
  EXPECT_TRUE(probe.stopped()) << "failed renderer left a live descendant";
}

TEST_F(ProcessRendererTest, TimedOutRendererDoesNotLeaveDescendantRunning) {
  DescendantProbe probe;
  ASSERT_TRUE(probe.open());
  const auto result = render(probe.mode("timeout"), std::chrono::seconds(1));
  EXPECT_THAT(result.error, testing::HasSubstr("deadline exceeded"));
  EXPECT_TRUE(probe.stopped()) << "timed-out renderer left a live descendant";
}

TEST_F(ProcessRendererTest, NonExecutableRendererReportsExecStageAndErrno) {
  const auto blocked = root_ / "not-executable";
  std::ofstream(blocked) << "not an executable\n";
  std::error_code error;
  std::filesystem::permissions(
      blocked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error;
  const auto renderer = MakeProcessRenderer({blocked, root_, {}, std::chrono::seconds(2), true});
  const auto result = renderer(test_, 1, 1);
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("renderer exit 127"));
  EXPECT_THAT(result.error,
              testing::HasSubstr("renderer setup stage=execv errno=" + std::to_string(EACCES)));
}

TEST_F(ProcessRendererTest, NonzeroExitIsAnError) {
  EXPECT_THAT(render("failure").error, testing::HasSubstr("renderer exit 7"));
}

TEST_F(ProcessRendererTest, MissingOutputCannotPass) {
  const auto result = render("missing");
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::Not(testing::IsEmpty()));
}

TEST_F(ProcessRendererTest, DeadlineKillsAndCollectsChild) {
  const auto start = std::chrono::steady_clock::now();
  const auto result = render("timeout", std::chrono::milliseconds(50));
  EXPECT_THAT(result.error, testing::HasSubstr("deadline exceeded"));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST_F(ProcessRendererTest, DiagnosticsAreBounded) {
  const auto result = render("diagnostics");
  EXPECT_THAT(result.error, testing::HasSubstr("renderer exit 7"));
  EXPECT_LE(result.error.size(), 16416u);
}

#if defined(__linux__)
TEST_F(ProcessRendererTest, AddressSpaceLimitRejectsAllocation) {
  EXPECT_THAT(render("memory").error, testing::HasSubstr("renderer exit 7"));
}
#else
TEST_F(ProcessRendererTest, UntrustedMemoryModeFailsClosed) {
  const auto renderer = MakeProcessRenderer({executable, root_, {}, std::chrono::seconds(1)});
  EXPECT_THAT(renderer(test_, 1, 1).error, testing::HasSubstr("memory limits are unavailable"));
}
#endif

TEST_F(ProcessRendererTest, InvalidDeadlineIsRejected) {
  EXPECT_THAT(render("success", std::chrono::milliseconds(0)).error,
              testing::HasSubstr("timeout must be"));
}

TEST_F(ProcessRendererTest, SymlinkOutputCannotEscape) {
  EXPECT_THAT(render("symlink").error, testing::HasSubstr("not a symlink"));
}

}  // namespace
}  // namespace donner::svg_suite

namespace {

int writeFixtureImage(const char* output, const char* input) {
  if (!std::string_view(input).ends_with("input {root} {output} ; literal.svg")) {
    return 9;
  }
  const std::vector<uint8_t> pixels{255, 0, 0, 255};
  return donner::svg_suite::WritePng(output, {1, 1, 1, pixels}).empty() ? 0 : 10;
}

int descendantFixture(std::string_view mode, const char* output, const char* input) {
  const auto separator = mode.find(':');
  const int control = std::stoi(std::string(mode.substr(separator + 1)));
  const pid_t child = fork();
  if (child < 0) {
    return 13;
  }
  if (child == 0) {
    for (;;) {
      pause();
    }
  }
  if (write(control, &child, sizeof(child)) != static_cast<ssize_t>(sizeof(child))) {
    kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    return 14;
  }
  close(control);
  if (mode.starts_with("descendant-timeout:")) {
    for (;;) {
      pause();
    }
  }
  if (mode.starts_with("descendant-failure:")) {
    return 7;
  }
  return writeFixtureImage(output, input);
}

int runFixture(std::string_view mode, const char* output, const char* input) {
  if (mode.starts_with("descendant-")) {
    return descendantFixture(mode, output, input);
  }
  if (mode == "memory") {
    void* memory = std::malloc(static_cast<size_t>(2) * 1024u * 1024u * 1024u);
    if (memory != nullptr) {
      std::free(memory);
      return 12;
    }
    return 7;
  }
  if (mode == "failure") {
    return 7;
  }
  if (mode == "missing") {
    return 0;
  }
  if (mode == "timeout") {
    for (;;) {
      poll(nullptr, 0, 1000);
    }
  }
  if (mode == "diagnostics") {
    for (int i = 0; i < 100; ++i) {
      std::cerr << std::string(1024, 'x');
    }
    return 7;
  }
  if (mode == "symlink") {
    std::error_code error;
    std::filesystem::create_symlink("/dev/null", output, error);
    return error ? 8 : 0;
  }
  if (mode == "success") {
    return writeFixtureImage(output, input);
  }
  return 11;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string_view(argv[1]) == "--fixture") {
    return runFixture(argv[2], argv[3], argv[4]);
  }
  donner::svg_suite::executable = std::filesystem::canonical(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
