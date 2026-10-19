#include "tools/svg_suite/ProcessRenderer.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "tools/svg_suite/ProcessRendererInternal.h"
#include "tools/svg_suite/TextEncoding.h"

namespace donner::svg_suite {
namespace {

constexpr size_t kMaxDiagnostics = 16u * 1024u;

struct ChildResult {
  bool success = false;
  std::string diagnostics;
};

std::optional<std::string> expandArgument(const std::string& value,
                                          const std::map<std::string, std::string>& tokens) {
  std::string result;
  size_t offset = 0;
  // Inspect only the original template; inserted paths may themselves contain token syntax.
  while (offset < value.size()) {
    if (value[offset] == '{') {
      const size_t end = value.find('}', offset + 1);
      if (end != std::string::npos) {
        const auto token = tokens.find(value.substr(offset, end - offset + 1));
        if (token != tokens.end()) {
          if (token->second.size() > kMaxIdentityBytes - result.size()) {
            return std::nullopt;
          }
          result += token->second;
          offset = end + 1;
          continue;
        }
      }
    }
    if (result.size() == kMaxIdentityBytes) {
      return std::nullopt;
    }
    result += value[offset++];
  }
  return result;
}

void readDiagnostics(int descriptor, std::string& diagnostics) {
  std::array<char, 4096> buffer{};
  // Bound each drain so a continuously writing child cannot hide its deadline.
  for (int i = 0; i < 8; ++i) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count <= 0) {
      return;
    }
    const size_t remaining = kMaxDiagnostics - diagnostics.size();
    diagnostics.append(buffer.data(), std::min(static_cast<size_t>(count), remaining));
  }
}

void writeChildDiagnostic(int descriptor, std::string_view text) {
  while (!text.empty()) {
    const ssize_t count = write(descriptor, text.data(), text.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return;
    }
    text.remove_prefix(static_cast<size_t>(count));
  }
}

[[noreturn]] void failChildSetup(int descriptor, std::string_view stage, int error, int exitCode) {
  // No allocation or libc formatting is safe between fork and exec in a threaded caller.
  char digits[16];
  size_t start = sizeof(digits);
  unsigned int remaining = static_cast<unsigned int>(error);
  do {
    digits[--start] = static_cast<char>('0' + remaining % 10);
    remaining /= 10;
  } while (remaining != 0);
  writeChildDiagnostic(descriptor, "renderer setup stage=");
  writeChildDiagnostic(descriptor, stage);
  writeChildDiagnostic(descriptor, " errno=");
  writeChildDiagnostic(descriptor, {digits + start, sizeof(digits) - start});
  writeChildDiagnostic(descriptor, "\n");
  _exit(exitCode);
}

[[noreturn]] void executeChild(const std::vector<char*>& argv, const std::string& directory,
                               int readFd, int writeFd, int timeoutSeconds,
                               size_t memoryLimitBytes) {
  close(readFd);
  if (setpgid(0, 0) != 0) {
    failChildSetup(writeFd, "setpgid", errno, 126);
  }
  if (dup2(writeFd, STDOUT_FILENO) < 0) {
    failChildSetup(writeFd, "dup2_stdout", errno, 126);
  }
  if (dup2(writeFd, STDERR_FILENO) < 0) {
    failChildSetup(writeFd, "dup2_stderr", errno, 126);
  }
  if (chdir(directory.c_str()) != 0) {
    failChildSetup(writeFd, "chdir", errno, 126);
  }
  const rlimit fileLimit{32u * 1024u * 1024u, 32u * 1024u * 1024u};
  const rlimit coreLimit{0, 0};
  const rlimit cpuLimit{static_cast<rlim_t>(timeoutSeconds),
                        static_cast<rlim_t>(timeoutSeconds + 1)};
  if (setrlimit(RLIMIT_FSIZE, &fileLimit) != 0) {
    failChildSetup(writeFd, "setrlimit_fsize", errno, 126);
  }
  if (setrlimit(RLIMIT_CORE, &coreLimit) != 0) {
    failChildSetup(writeFd, "setrlimit_core", errno, 126);
  }
  if (setrlimit(RLIMIT_CPU, &cpuLimit) != 0) {
    failChildSetup(writeFd, "setrlimit_cpu", errno, 126);
  }
#if defined(__linux__)
  const rlimit memoryLimit{static_cast<rlim_t>(memoryLimitBytes),
                           static_cast<rlim_t>(memoryLimitBytes)};
  if (setrlimit(RLIMIT_AS, &memoryLimit) != 0) {
    failChildSetup(writeFd, "setrlimit_as", errno, 126);
  }
#else
  (void)memoryLimitBytes;
#endif
  execv(argv[0], argv.data());
  failChildSetup(writeFd, "execv", errno, 127);
}

struct DiagnosticPipe {
  int readFd = -1;
  int writeFd = -1;
};

bool openDiagnosticPipe(DiagnosticPipe& pipe) {
  int descriptors[2];
  if (::pipe(descriptors) != 0) {
    return false;
  }
  if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK) != 0 ||
      fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) != 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  pipe = {descriptors[0], descriptors[1]};
  return true;
}

struct WaitResult {
  bool timedOut = false;
  bool failed = false;
  bool ownershipLost = false;
};

WaitResult waitForChild(pid_t child, int diagnosticFd, std::chrono::milliseconds timeout,
                        std::string& diagnostics) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool pipeClosed = false;
  for (;;) {
    readDiagnostics(diagnosticFd, diagnostics);
    siginfo_t information{};
    // Reserve the leader PID until group cleanup finishes. Reaping first would permit PID reuse.
    if (waitid(P_PID, static_cast<id_t>(child), &information, WEXITED | WNOHANG | WNOWAIT) != 0) {
      if (errno != EINTR) {
        return {false, true, errno == ECHILD};
      }
    } else if (information.si_pid == child) {
      return {};
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      return {true, false};
    }
    pollfd descriptor{pipeClosed ? -1 : diagnosticFd, POLLIN, 0};
    poll(&descriptor, 1, static_cast<int>(std::min<int64_t>(remaining.count(), 10)));
    pipeClosed = pipeClosed || (descriptor.revents & POLLHUP) != 0;
  }
}

struct PosixChildOperations {
  void killProcessOrGroup(pid_t target) { kill(target, SIGKILL); }

  internal::ChildObservation observeExit(pid_t child) {
    siginfo_t information{};
    int observed;
    do {
      observed = waitid(P_PID, static_cast<id_t>(child), &information, WEXITED | WNOWAIT);
    } while (observed < 0 && errno == EINTR);
    if (observed < 0) {
      return errno == ECHILD ? internal::ChildObservation::OwnershipLost
                             : internal::ChildObservation::Failed;
    }
    return information.si_pid == child ? internal::ChildObservation::Observed
                                       : internal::ChildObservation::Failed;
  }

  bool reapChild(pid_t child, int& status) {
    pid_t collected;
    do {
      collected = waitpid(child, &status, 0);
    } while (collected < 0 && errno == EINTR);
    return collected == child;
  }
};

ChildResult childOutcome(const WaitResult& waited, bool collected, int status,
                         std::string diagnostics) {
  if (waited.ownershipLost) {
    return {false, "renderer child ownership lost; cleanup signals skipped\n" + diagnostics};
  }
  if (waited.timedOut) {
    return {false, "renderer deadline exceeded\n" + diagnostics};
  }
  if (waited.failed || !collected) {
    return {false, "cannot collect renderer status\n" + diagnostics};
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const std::string outcome = WIFSIGNALED(status)
                                    ? "renderer signal " + std::to_string(WTERMSIG(status))
                                    : "renderer exit " + std::to_string(WEXITSTATUS(status));
    return {false, outcome + "\n" + diagnostics};
  }
  return {true, std::move(diagnostics)};
}

std::string checkChildReapingPolicy() {
  struct sigaction disposition{};
  if (sigaction(SIGCHLD, nullptr, &disposition) != 0) {
    return "cannot query renderer SIGCHLD disposition";
  }
  if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT) != 0) {
    return "renderer requires waitable children: SIGCHLD must not use SIG_IGN or SA_NOCLDWAIT";
  }
  return {};
}

ChildResult runChild(std::vector<std::string> arguments, const std::filesystem::path& directory,
                     std::chrono::milliseconds timeout, size_t memoryLimitBytes) {
  const auto reapingPolicy = checkChildReapingPolicy();
  if (!reapingPolicy.empty()) {
    return {false, reapingPolicy};
  }
  std::vector<char*> argv;
  for (auto& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  const std::string childDirectory = directory.string();
  DiagnosticPipe pipe;
  if (!openDiagnosticPipe(pipe)) {
    return {false, "cannot create or configure renderer diagnostic pipe"};
  }
  const pid_t child = fork();
  if (child == 0) {
    executeChild(argv, childDirectory, pipe.readFd, pipe.writeFd,
                 static_cast<int>((timeout.count() + 999) / 1000), memoryLimitBytes);
  }
  close(pipe.writeFd);
  if (child < 0) {
    close(pipe.readFd);
    return {false, "cannot create renderer child"};
  }
  // Only the child sets its group. Concurrent parent/child setpgid can fail on supported hosts;
  // deadline cleanup also signals the direct child if its group has not been established yet.
  std::string diagnostics;
  const auto waited = waitForChild(child, pipe.readFd, timeout, diagnostics);
  int status = 0;
  PosixChildOperations operations;
  const bool collected = internal::FinishChild(child, waited.ownershipLost, status, operations);
  readDiagnostics(pipe.readFd, diagnostics);
  close(pipe.readFd);
  return childOutcome(waited, collected, status, std::move(diagnostics));
}

std::string validateProcessText(const ProcessRendererOptions& options, const StaticCase& test) {
  if (!Utf8Path(options.executable) || !Utf8Path(options.workRoot) ||
      !ArgumentBytes(options.arguments)) {
    return "renderer paths and arguments require bounded NUL-free UTF-8";
  }
  for (const auto* path : {&test.root, &test.svgPath, &test.expectedPath}) {
    if (!Utf8Path(*path)) {
      return "renderer case paths require bounded NUL-free UTF-8";
    }
  }
  return {};
}

std::string validateProcessLimits(const ProcessRendererOptions& options) {
  if (options.timeout.count() <= 0 || options.timeout.count() > 10000) {
    return "renderer timeout must be in (0, 10000] milliseconds";
  }
  if (options.memoryLimitBytes < 64u * 1024u * 1024u ||
      options.memoryLimitBytes > static_cast<size_t>(4) * 1024u * 1024u * 1024u) {
    return "renderer memory limit must be between 64 MiB and 4 GiB";
  }
#if !defined(__linux__)
  if (!options.trustedInputs) {
    return "renderer memory limits are unavailable on this host; "
           "only explicitly trusted input runs are supported";
  }
#endif
  return {};
}

std::string validateProcess(const ProcessRendererOptions& options, const StaticCase& test) {
  const auto encoding = validateProcessText(options, test);
  if (!encoding.empty()) {
    return encoding;
  }
  if (test.iteration && *test.iteration == 0) {
    return "case iteration must be positive";
  }
  const auto limits = validateProcessLimits(options);
  if (!limits.empty()) {
    return limits;
  }
  if (test.id.empty() ||
      test.id.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
    return "invalid case identity";
  }
  return {};
}

struct ProcessPaths {
  std::filesystem::path executable;
  std::filesystem::path directory;
  std::string error;
};

ProcessPaths prepareProcessPaths(const ProcessRendererOptions& options, const StaticCase& test) {
  std::error_code error;
  const auto executable = std::filesystem::canonical(options.executable, error);
  if (error || !Utf8Path(executable) || !std::filesystem::is_regular_file(executable, error)) {
    return {{}, {}, "renderer executable is unavailable"};
  }
  std::filesystem::create_directories(options.workRoot, error);
  if (error) {
    return {{}, {}, "cannot create renderer work root"};
  }
  const auto workRoot = std::filesystem::canonical(options.workRoot, error);
  if (error || !Utf8Path(workRoot)) {
    return {{}, {}, "cannot resolve bounded UTF-8 renderer work root"};
  }
  const auto directory = CaseOutputDirectory(workRoot, test) / "process";
  if (std::filesystem::exists(directory, error) || error ||
      !std::filesystem::create_directories(directory, error) || error) {
    return {{}, {}, "renderer work directory exists or cannot be created"};
  }
  return {executable, directory, {}};
}

std::optional<std::vector<std::string>> processArguments(const ProcessRendererOptions& options,
                                                         const StaticCase& test,
                                                         const ProcessPaths& paths, int width,
                                                         int height) {
  std::error_code error;
  const auto& directory = paths.directory;
  const auto& executable = paths.executable;
  const std::filesystem::path output = directory / "render.png";
  const auto fonts = test.root / "fonts";
  const std::map<std::string, std::string> tokens{
      {"{input}", test.svgPath.string()},
      {"{output}", output.string()},
      {"{root}", test.root.string()},
      {"{fonts}", std::filesystem::is_directory(fonts, error) ? fonts.string() : ""},
      {"{width}", std::to_string(width)},
      {"{height}", std::to_string(height)}};
  std::vector<std::string> arguments{executable.string()};
  size_t bytes = arguments.front().size();
  for (const auto& argument : options.arguments) {
    auto expanded = expandArgument(argument, tokens);
    if (!expanded || expanded->size() > kMaxArgumentBytes - bytes) {
      return std::nullopt;
    }
    bytes += expanded->size();
    arguments.push_back(std::move(*expanded));
  }
  return arguments;
}

std::string preserveProcessEvidence(const ProcessRendererOptions& options, const StaticCase& test,
                                    const ProcessPaths& paths,
                                    const std::vector<std::string>& arguments,
                                    const ChildResult& outcome) {
  nlohmann::json metadata = {
      {"executable", *Utf8Path(paths.executable)},      {"arguments", arguments},
      {"work_directory", *Utf8Path(paths.directory)},   {"timeout_ms", options.timeout.count()},
      {"memory_limit_bytes", options.memoryLimitBytes}, {"diagnostics_path", "renderer.log"}};
#if defined(__linux__)
  metadata["memory_enforcement"] = "rlimit_as";
#else
  metadata["memory_enforcement"] = "unavailable_trusted_inputs";
#endif
  metadata["iteration"] =
      test.iteration ? nlohmann::json(*test.iteration) : nlohmann::json(nullptr);
  metadata["succeeded"] = outcome.success;
  std::ofstream record(paths.directory / "process.json");
  record << metadata.dump(2) << '\n';
  record.close();
  if (!record) {
    return "cannot preserve renderer process metadata";
  }
  std::ofstream diagnostics(paths.directory / "renderer.log");
  diagnostics << outcome.diagnostics;
  diagnostics.close();
  return diagnostics ? "" : "cannot preserve renderer diagnostics";
}

ImageResult renderProcess(const ProcessRendererOptions& options, const StaticCase& test, int width,
                          int height) {
  const auto validation = validateProcess(options, test);
  if (!validation.empty()) {
    return {std::nullopt, validation};
  }
  const auto paths = prepareProcessPaths(options, test);
  if (!paths.error.empty()) {
    return {std::nullopt, paths.error};
  }
  const auto arguments = processArguments(options, test, paths, width, height);
  if (!arguments || !ArgumentBytes(*arguments) || !Utf8Path(paths.directory)) {
    return {std::nullopt, "expanded renderer arguments require bounded NUL-free UTF-8"};
  }
  const auto outcome =
      runChild(*arguments, paths.directory, options.timeout, options.memoryLimitBytes);
  const auto evidenceError = preserveProcessEvidence(options, test, paths, *arguments, outcome);
  if (!evidenceError.empty()) {
    return {std::nullopt, evidenceError};
  }
  if (!outcome.success) {
    return {std::nullopt, outcome.diagnostics};
  }
  std::error_code error;
  const auto output = paths.directory / "render.png";
  if (std::filesystem::is_symlink(output, error) || error) {
    return {std::nullopt, "renderer output must be a regular PNG, not a symlink"};
  }
  return ReadPng(output);
}

}  // namespace

Renderer MakeProcessRenderer(ProcessRendererOptions options) {
  return [options = std::move(options)](const StaticCase& test, int width, int height) {
    return renderProcess(options, test, width, height);
  };
}

}  // namespace donner::svg_suite
