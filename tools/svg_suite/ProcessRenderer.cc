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
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace donner::svg_suite {
namespace {

constexpr size_t kMaxDiagnostics = 16u * 1024u;

struct ChildResult {
  bool success = false;
  std::string diagnostics;
};

std::string expandArgument(std::string value, const std::map<std::string, std::string>& tokens) {
  for (const auto& [token, replacement] : tokens) {
    size_t offset = 0;
    while ((offset = value.find(token, offset)) != std::string::npos) {
      value.replace(offset, token.size(), replacement);
      offset += replacement.size();
    }
  }
  return value;
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

[[noreturn]] void executeChild(const std::vector<char*>& argv, const std::string& directory,
                               int readFd, int writeFd, int timeoutSeconds,
                               size_t memoryLimitBytes) {
  close(readFd);
  if (setpgid(0, 0) != 0 || dup2(writeFd, STDOUT_FILENO) < 0 || dup2(writeFd, STDERR_FILENO) < 0 ||
      chdir(directory.c_str()) != 0) {
    _exit(126);
  }
  close(writeFd);
  const rlimit fileLimit{32u * 1024u * 1024u, 32u * 1024u * 1024u};
  const rlimit coreLimit{0, 0};
  const rlimit cpuLimit{static_cast<rlim_t>(timeoutSeconds),
                        static_cast<rlim_t>(timeoutSeconds + 1)};
  if (setrlimit(RLIMIT_FSIZE, &fileLimit) != 0 || setrlimit(RLIMIT_CORE, &coreLimit) != 0 ||
      setrlimit(RLIMIT_CPU, &cpuLimit) != 0) {
    _exit(126);
  }
#if defined(__linux__)
  const rlimit memoryLimit{static_cast<rlim_t>(memoryLimitBytes),
                           static_cast<rlim_t>(memoryLimitBytes)};
  if (setrlimit(RLIMIT_AS, &memoryLimit) != 0) {
    _exit(126);
  }
#else
  (void)memoryLimitBytes;
#endif
  execv(argv[0], argv.data());
  _exit(127);
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
  if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK) != 0) {
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
        return {false, true};
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

bool terminateAndCollect(pid_t child, int& status) {
  // executeChild establishes its group before exec, so even a successful renderer may leave
  // descendants in this group. A parent's setpgid result is not proof the group does not exist:
  // ESRCH can mean the leader has already exited. Kill the group on every completion path.
  kill(-child, SIGKILL);
  // The direct child can still be before setpgid when its deadline expires.
  kill(child, SIGKILL);
  siginfo_t information{};
  int observed;
  do {
    observed = waitid(P_PID, static_cast<id_t>(child), &information, WEXITED | WNOWAIT);
  } while (observed < 0 && errno == EINTR);
  // If the initial group signal raced with child-side setpgid, the now-dead leader cannot spawn
  // another descendant. Repeat group cleanup while its PID is still reserved.
  kill(-child, SIGKILL);
  pid_t collected;
  do {
    collected = waitpid(child, &status, 0);
  } while (collected < 0 && errno == EINTR);
  return collected == child;
}

ChildResult childOutcome(const WaitResult& waited, bool collected, int status,
                         std::string diagnostics) {
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

ChildResult runChild(std::vector<std::string> arguments, const std::filesystem::path& directory,
                     std::chrono::milliseconds timeout, size_t memoryLimitBytes) {
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
  // Both sides attempt setup to close the pre-exec race. Child-side setup remains mandatory;
  // EACCES/ESRCH here may simply indicate that a fast renderer has already executed or exited.
  setpgid(child, child);
  std::string diagnostics;
  const auto waited = waitForChild(child, pipe.readFd, timeout, diagnostics);
  int status = 0;
  const bool collected = terminateAndCollect(child, status);
  readDiagnostics(pipe.readFd, diagnostics);
  close(pipe.readFd);
  return childOutcome(waited, collected, status, std::move(diagnostics));
}

std::string validateProcess(const ProcessRendererOptions& options, const StaticCase& test) {
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
  if (error || !std::filesystem::is_regular_file(executable, error)) {
    return {{}, {}, "renderer executable is unavailable"};
  }
  std::filesystem::create_directories(options.workRoot, error);
  if (error) {
    return {{}, {}, "cannot create renderer work root"};
  }
  const auto workRoot = std::filesystem::canonical(options.workRoot, error);
  if (error) {
    return {{}, {}, "cannot resolve renderer work root"};
  }
  std::filesystem::path directory = workRoot;
  for (size_t offset = 0; offset < test.id.size(); offset += 120) {
    directory /= test.id.substr(offset, 120);
  }
  directory /= "process";
  if (std::filesystem::exists(directory, error) || error ||
      !std::filesystem::create_directories(directory, error) || error) {
    return {{}, {}, "renderer work directory exists or cannot be created"};
  }
  return {executable, directory, {}};
}

std::vector<std::string> processArguments(const ProcessRendererOptions& options,
                                          const StaticCase& test, const ProcessPaths& paths,
                                          int width, int height) {
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
  for (const auto& argument : options.arguments) {
    arguments.push_back(expandArgument(argument, tokens));
  }
  return arguments;
}

std::string preserveProcessEvidence(const ProcessRendererOptions& options,
                                    const ProcessPaths& paths,
                                    const std::vector<std::string>& arguments,
                                    const ChildResult& outcome) {
  nlohmann::json metadata = {
      {"executable", paths.executable.string()},        {"arguments", arguments},
      {"work_directory", paths.directory.string()},     {"timeout_ms", options.timeout.count()},
      {"memory_limit_bytes", options.memoryLimitBytes}, {"diagnostics_path", "renderer.log"}};
#if defined(__linux__)
  metadata["memory_enforcement"] = "rlimit_as";
#else
  metadata["memory_enforcement"] = "unavailable_trusted_inputs";
#endif
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
  const auto outcome =
      runChild(arguments, paths.directory, options.timeout, options.memoryLimitBytes);
  const auto evidenceError = preserveProcessEvidence(options, paths, arguments, outcome);
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
