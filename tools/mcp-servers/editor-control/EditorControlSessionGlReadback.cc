/// @file
/// GL-readback replay runner for the editor-control MCP: selects between the
/// in-process GL replay and a `bazel run` subprocess (for sandboxed servers
/// that cannot create a GL context), and owns the subprocess plumbing and
/// replay-result JSON parsing for the latter.

#include "tools/mcp-servers/editor-control/EditorControlSessionGlReadback.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/repro/GlRnrReplay.h"
#include "nlohmann/json.hpp"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

struct ProcessRunResult {
  bool ok = false;
  int exitCode = -1;
  bool timedOut = false;
  std::string stdoutText;
  std::string stderrText;
  std::string error;
};

std::string_view GlRnrReplayCropModeArgument(repro::GlRnrReplayCropMode cropMode) {
  switch (cropMode) {
    case repro::GlRnrReplayCropMode::Full: return "full";
    case repro::GlRnrReplayCropMode::RenderPane: return "render-pane";
    case repro::GlRnrReplayCropMode::DocumentCanvas: return "document-canvas";
  }
  return "full";
}

std::string_view GlRnrReplayWorkerSchedulingArgument(
    repro::GlRnrReplayWorkerScheduling scheduling) {
  switch (scheduling) {
    case repro::GlRnrReplayWorkerScheduling::Realtime: return "realtime";
    case repro::GlRnrReplayWorkerScheduling::DrainEachFrame: return "drain-each-frame";
    case repro::GlRnrReplayWorkerScheduling::HoldFramesBehind: return "hold-frames-behind";
  }
  return "realtime";
}

void CloseFd(int* fd) {
  if (*fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}

void ClosePipe(int pipeFds[2]) {
  CloseFd(&pipeFds[0]);
  CloseFd(&pipeFds[1]);
}

void WriteChildError(std::string_view message) {
  while (!message.empty()) {
    const ssize_t bytesWritten = ::write(STDERR_FILENO, message.data(), message.size());
    if (bytesWritten > 0) {
      message.remove_prefix(static_cast<std::size_t>(bytesWritten));
    } else if (bytesWritten < 0 && errno == EINTR) {
      continue;
    } else {
      return;
    }
  }
}

bool SetFdFlag(int fd, int command, int flag, std::string* error) {
  const int existing = ::fcntl(fd, command, 0);
  if (existing < 0) {
    *error = std::string("fcntl failed: ") + std::strerror(errno);
    return false;
  }

  const int setCommand = command == F_GETFD ? F_SETFD : F_SETFL;
  if (::fcntl(fd, setCommand, existing | flag) < 0) {
    *error = std::string("fcntl failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool PreparePipe(int pipeFds[2], std::string* error) {
  if (::pipe(pipeFds) < 0) {
    *error = std::string("pipe failed: ") + std::strerror(errno);
    return false;
  }
  if (!SetFdFlag(pipeFds[0], F_GETFD, FD_CLOEXEC, error) ||
      !SetFdFlag(pipeFds[1], F_GETFD, FD_CLOEXEC, error) ||
      !SetFdFlag(pipeFds[0], F_GETFL, O_NONBLOCK, error)) {
    ClosePipe(pipeFds);
    return false;
  }
  return true;
}

void ReadAvailableFd(int fd, std::string* output, bool* isOpen) {
  char buffer[4096];
  while (true) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      output->append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) {
      *isOpen = false;
      ::close(fd);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    *isOpen = false;
    ::close(fd);
    return;
  }
}

void PollChildExit(pid_t childPid, bool* childRunning, int* exitCode) {
  if (!*childRunning) {
    return;
  }

  int status = 0;
  const pid_t waitResult = ::waitpid(childPid, &status, WNOHANG);
  if (waitResult == 0) {
    return;
  }
  if (waitResult < 0) {
    if (errno == ECHILD) {
      *childRunning = false;
    }
    return;
  }

  *childRunning = false;
  if (WIFEXITED(status)) {
    *exitCode = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    *exitCode = -WTERMSIG(status);
  }
}

std::string TailText(std::string_view text, std::size_t maxChars) {
  if (text.size() <= maxChars) {
    return std::string(text);
  }
  return std::string(text.substr(text.size() - maxChars));
}

ProcessRunResult RunProcess(std::span<const std::string> args, std::chrono::milliseconds timeout,
                            std::string_view workingDirectory = "") {
  ProcessRunResult result;
  if (args.empty()) {
    result.error = "empty command";
    return result;
  }

  int stdoutPipe[2] = {-1, -1};
  int stderrPipe[2] = {-1, -1};
  if (!PreparePipe(stdoutPipe, &result.error)) {
    return result;
  }
  if (!PreparePipe(stderrPipe, &result.error)) {
    ClosePipe(stdoutPipe);
    return result;
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  const std::string workingDirectoryPath(workingDirectory);

  const pid_t childPid = ::fork();
  if (childPid < 0) {
    result.error = std::string("fork failed: ") + std::strerror(errno);
    ClosePipe(stdoutPipe);
    ClosePipe(stderrPipe);
    return result;
  }

  if (childPid == 0) {
    ::setpgid(0, 0);
    ::dup2(stdoutPipe[1], STDOUT_FILENO);
    ::dup2(stderrPipe[1], STDERR_FILENO);
    ClosePipe(stdoutPipe);
    ClosePipe(stderrPipe);
    if (!workingDirectoryPath.empty() && ::chdir(workingDirectoryPath.c_str()) < 0) {
      constexpr std::string_view kChdirFailure = "chdir failed\n";
      WriteChildError(kChdirFailure);
      ::_exit(126);
    }
    ::execvp(argv[0], argv.data());
    constexpr std::string_view kExecFailure = "execvp failed\n";
    WriteChildError(kExecFailure);
    ::_exit(127);
  }

  CloseFd(&stdoutPipe[1]);
  CloseFd(&stderrPipe[1]);

  bool stdoutOpen = true;
  bool stderrOpen = true;
  bool childRunning = true;
  bool sentTerminate = false;
  bool sentKill = false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto terminateDeadline = deadline;

  while (stdoutOpen || stderrOpen || childRunning) {
    PollChildExit(childPid, &childRunning, &result.exitCode);
    if (!childRunning) {
      if (stdoutOpen) {
        ReadAvailableFd(stdoutPipe[0], &result.stdoutText, &stdoutOpen);
        if (stdoutOpen) {
          CloseFd(&stdoutPipe[0]);
        }
        stdoutOpen = false;
      }
      if (stderrOpen) {
        ReadAvailableFd(stderrPipe[0], &result.stderrText, &stderrOpen);
        if (stderrOpen) {
          CloseFd(&stderrPipe[0]);
        }
        stderrOpen = false;
      }
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    if (childRunning && now >= deadline && !sentTerminate) {
      result.timedOut = true;
      sentTerminate = true;
      terminateDeadline = now + std::chrono::seconds(2);
      ::kill(-childPid, SIGTERM);
    }
    if (childRunning && sentTerminate && now >= terminateDeadline && !sentKill) {
      sentKill = true;
      ::kill(-childPid, SIGKILL);
    }

    std::array<pollfd, 2> fds{};
    nfds_t pollCount = 0;
    if (stdoutOpen) {
      fds[pollCount++] = pollfd{.fd = stdoutPipe[0], .events = POLLIN | POLLHUP | POLLERR};
    }
    if (stderrOpen) {
      fds[pollCount++] = pollfd{.fd = stderrPipe[0], .events = POLLIN | POLLHUP | POLLERR};
    }

    if (pollCount > 0) {
      const int pollResult = ::poll(fds.data(), pollCount, 50);
      if (pollResult > 0) {
        for (nfds_t i = 0; i < pollCount; ++i) {
          if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            continue;
          }
          if (fds[i].fd == stdoutPipe[0]) {
            ReadAvailableFd(stdoutPipe[0], &result.stdoutText, &stdoutOpen);
          } else if (fds[i].fd == stderrPipe[0]) {
            ReadAvailableFd(stderrPipe[0], &result.stderrText, &stderrOpen);
          }
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (result.timedOut) {
    result.error = "command timed out after " + std::to_string(timeout.count()) + "ms";
    if (!result.stderrText.empty()) {
      result.error += "\nstderr:\n" + TailText(result.stderrText, 4096);
    }
    if (!result.stdoutText.empty()) {
      result.error += "\nstdout:\n" + TailText(result.stdoutText, 4096);
    }
    return result;
  }
  if (result.exitCode != 0) {
    result.error = "command exited with code " + std::to_string(result.exitCode) + "\nstderr:\n" +
                   TailText(result.stderrText, 4096);
    return result;
  }

  result.ok = true;
  return result;
}

std::optional<json> ParseJsonObjectFromStdout(std::string_view stdoutText, std::string* error) {
  json parsed = json::parse(stdoutText.begin(), stdoutText.end(), nullptr, false);
  if (parsed.is_object()) {
    return parsed;
  }

  std::vector<std::string> lines;
  std::istringstream stream{std::string(stdoutText)};
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    parsed = json::parse(it->begin(), it->end(), nullptr, false);
    if (parsed.is_object()) {
      return parsed;
    }
  }

  *error = "helper did not print a JSON object\nstdout:\n" + TailText(stdoutText, 4096);
  return std::nullopt;
}

std::optional<std::string> ParseBazelPertinentWorkspace(std::string_view stderrText) {
  constexpr std::string_view kMarker = "pertinent workspace directory is: '";
  const std::size_t markerPos = stderrText.find(kMarker);
  if (markerPos == std::string_view::npos) {
    return std::nullopt;
  }

  const std::size_t pathBegin = markerPos + kMarker.size();
  const std::size_t pathEnd = stderrText.find('\'', pathBegin);
  if (pathEnd == std::string_view::npos || pathEnd <= pathBegin) {
    return std::nullopt;
  }

  return std::string(stderrText.substr(pathBegin, pathEnd - pathBegin));
}

bool ReadJsonUint64Member(const json& object, std::string_view key, std::uint64_t* out,
                          std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_integer()) {
    *error = "helper JSON missing integer field: " + std::string(key);
    return false;
  }
  if (it->is_number_unsigned()) {
    *out = it->get<std::uint64_t>();
    return true;
  }

  const int64_t value = it->get<int64_t>();
  if (value < 0) {
    *error = "helper JSON field must be non-negative: " + std::string(key);
    return false;
  }
  *out = static_cast<std::uint64_t>(value);
  return true;
}

bool ReadJsonStringMember(const json& object, std::string_view key, std::string* out,
                          std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    *error = "helper JSON missing string field: " + std::string(key);
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool ParseBazelGlRnrReplayResult(const json& object, repro::GlRnrReplayResult* result,
                                 std::string* error) {
  const auto capturesIt = object.find("captures");
  if (capturesIt == object.end() || !capturesIt->is_array()) {
    *error = "helper JSON missing captures array";
    return false;
  }

  result->captures.clear();
  for (const json& captureJson : *capturesIt) {
    if (!captureJson.is_object()) {
      *error = "helper JSON capture entry must be an object";
      return false;
    }

    repro::GlRnrReplayCapture capture;
    std::string path;
    if (!ReadJsonUint64Member(captureJson, "frame", &capture.frameIndex, error) ||
        !ReadJsonStringMember(captureJson, "reason", &capture.reason, error) ||
        !ReadJsonStringMember(captureJson, "path", &path, error)) {
      return false;
    }
    capture.path = std::filesystem::path(path);
    result->captures.push_back(std::move(capture));
  }

  return true;
}

std::vector<std::string> BazelGlRnrReplayCommand(const repro::GlRnrReplayOptions& options) {
  std::vector<std::string> args{
      "bazel",
      "run",
      "--noshow_progress",
      "--noshow_loading_progress",
      "--color=no",
      "//donner/editor/tests:editor_rnr_gl_replay",
      "--",
      "--rnr",
      options.rnrPath.string(),
      "--out-dir",
      options.outputDir.string(),
      "--crop",
      std::string(GlRnrReplayCropModeArgument(options.cropMode)),
  };

  if (options.svgPathOverride.has_value()) {
    args.push_back("--svg");
    args.push_back(options.svgPathOverride->string());
  }
  for (const std::uint64_t frame : options.captureFrames) {
    args.push_back("--capture-frame");
    args.push_back(std::to_string(frame));
  }
  if (options.captureLeftMouseDownOrdinal.has_value()) {
    args.push_back("--capture-left-mousedown");
    args.push_back(std::to_string(*options.captureLeftMouseDownOrdinal));
  }
  if (options.maxFrame.has_value()) {
    args.push_back("--max-frame");
    args.push_back(std::to_string(*options.maxFrame));
  }
  if (options.visible) {
    args.push_back("--visible");
  }
  if (!options.pace) {
    args.push_back("--no-pace");
  }
  if (options.driveDocumentSpaceInput) {
    args.push_back("--drive-document-input");
  }
  if (options.workerRenderDelayMsForTesting > 0) {
    args.push_back("--worker-delay-ms");
    args.push_back(std::to_string(options.workerRenderDelayMsForTesting));
  }
  if (options.workerScheduling != repro::GlRnrReplayWorkerScheduling::Realtime) {
    args.push_back("--worker-scheduling");
    args.push_back(std::string(GlRnrReplayWorkerSchedulingArgument(options.workerScheduling)));
  }
  if (options.holdFramesBehind > 0) {
    args.push_back("--hold-frames-behind");
    args.push_back(std::to_string(options.holdFramesBehind));
  }

  return args;
}

}  // namespace

std::string_view GlReadbackRunnerName(GlReadbackRunner runner) {
  switch (runner) {
    case GlReadbackRunner::InProcess: return "in_process";
    case GlReadbackRunner::BazelRun: return "bazel_run";
  }
  return "in_process";
}

bool SelectGlReadbackRunner(GlReadbackRunner* runner, std::string* error) {
  if (const char* configured = std::getenv("DONNER_EDITOR_CONTROL_GL_READBACK_RUNNER")) {
    const std::string_view value(configured);
    if (value == "bazel" || value == "bazel_run") {
      *runner = GlReadbackRunner::BazelRun;
      return true;
    }
    if (value == "in_process" || value == "direct") {
      *runner = GlReadbackRunner::InProcess;
      return true;
    }

    *error = "DONNER_EDITOR_CONTROL_GL_READBACK_RUNNER must be bazel_run or in_process";
    return false;
  }

  const bool hasRunfilesEnvironment = std::getenv("RUNFILES_DIR") != nullptr ||
                                      std::getenv("RUNFILES_MANIFEST_FILE") != nullptr ||
                                      std::getenv("TEST_SRCDIR") != nullptr;
  *runner = hasRunfilesEnvironment ? GlReadbackRunner::InProcess : GlReadbackRunner::BazelRun;
  return true;
}

bool RunBazelGlRnrReplay(const repro::GlRnrReplayOptions& options,
                         std::chrono::milliseconds timeout, repro::GlRnrReplayResult* result,
                         std::string* error) {
  const std::vector<std::string> command = BazelGlRnrReplayCommand(options);
  ProcessRunResult processResult = RunProcess(command, timeout);
  if (!processResult.ok) {
    if (const std::optional<std::string> workspace =
            ParseBazelPertinentWorkspace(processResult.stderrText);
        workspace.has_value()) {
      processResult = RunProcess(command, timeout, *workspace);
    }
  }
  if (!processResult.ok) {
    *error = "failed to run GL replay helper through Bazel: " + processResult.error;
    if (!processResult.stdoutText.empty()) {
      *error += "\nstdout:\n" + TailText(processResult.stdoutText, 4096);
    }
    return false;
  }

  const std::optional<json> helperJson = ParseJsonObjectFromStdout(processResult.stdoutText, error);
  if (!helperJson.has_value()) {
    return false;
  }
  return ParseBazelGlRnrReplayResult(*helperJson, result, error);
}

}  // namespace donner::editor::mcp
