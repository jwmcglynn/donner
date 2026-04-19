#include "donner/editor/sandbox/SandboxHost.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/editor/sandbox/SandboxProtocol.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/Renderer.h"

extern "C" char** environ;  // POSIX, not declared in <unistd.h> on glibc.

namespace donner::editor::sandbox {

namespace {

/// Ensures SIGPIPE doesn't terminate the host if the child exits before
/// draining stdin. We want `write()` to return `EPIPE` instead. Installed
/// once per process; the static guard makes repeated construction cheap.
void IgnoreSigpipeOnce() {
  static const bool kInstalled = [] {
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    (void)::sigaction(SIGPIPE, &sa, nullptr);
    return true;
  }();
  (void)kInstalled;
}

SandboxStatus ClassifyExit(int rawStatus, int& outExit) {
  if (WIFEXITED(rawStatus)) {
    const int code = WEXITSTATUS(rawStatus);
    outExit = code;
    switch (code) {
      case kExitOk:
        return SandboxStatus::kOk;
      case kExitUsageError:
        return SandboxStatus::kUsageError;
      case kExitParseError:
        return SandboxStatus::kParseError;
      case kExitRenderError:
        return SandboxStatus::kRenderError;
      default:
        return SandboxStatus::kUnknownExit;
    }
  }
  if (WIFSIGNALED(rawStatus)) {
    outExit = -WTERMSIG(rawStatus);
    return SandboxStatus::kCrashed;
  }
  outExit = rawStatus;
  return SandboxStatus::kUnknownExit;
}

bool WriteAll(int fd, std::string_view data) {
  const char* p = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    const ssize_t n = ::write(fd, p, remaining);
    if (n > 0) {
      p += n;
      remaining -= static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;  // EPIPE, ENOSPC, or unexpected short write.
  }
  return true;
}

struct Pipe {
  int readFd = -1;
  int writeFd = -1;

  [[nodiscard]] bool valid() const { return readFd >= 0 && writeFd >= 0; }

  void closeRead() {
    if (readFd >= 0) {
      ::close(readFd);
      readFd = -1;
    }
  }
  void closeWrite() {
    if (writeFd >= 0) {
      ::close(writeFd);
      writeFd = -1;
    }
  }
  void closeBoth() {
    closeRead();
    closeWrite();
  }
};

Pipe MakePipe() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return Pipe{};
  }
  return Pipe{fds[0], fds[1]};
}

}  // namespace

SandboxHost::SandboxHost(std::string childBinaryPath)
    : childBinaryPath_(std::move(childBinaryPath)) {}

SandboxHost::RawExitInfo SandboxHost::spawnAndCollect(
    std::string_view svgBytes, int width, int height,
    std::vector<uint8_t>& outStdout) {
  IgnoreSigpipeOnce();

  RawExitInfo info;
  outStdout.clear();

  Pipe stdinPipe = MakePipe();
  Pipe stdoutPipe = MakePipe();
  Pipe stderrPipe = MakePipe();
  if (!stdinPipe.valid() || !stdoutPipe.valid() || !stderrPipe.valid()) {
    stdinPipe.closeBoth();
    stdoutPipe.closeBoth();
    stderrPipe.closeBoth();
    info.status = SandboxStatus::kSpawnFailed;
    info.diagnostics = "pipe() failed: ";
    info.diagnostics += std::strerror(errno);
    return info;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, stdinPipe.readFd, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stdoutPipe.writeFd, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stderrPipe.writeFd, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, stdinPipe.readFd);
  posix_spawn_file_actions_addclose(&actions, stdinPipe.writeFd);
  posix_spawn_file_actions_addclose(&actions, stdoutPipe.readFd);
  posix_spawn_file_actions_addclose(&actions, stdoutPipe.writeFd);
  posix_spawn_file_actions_addclose(&actions, stderrPipe.readFd);
  posix_spawn_file_actions_addclose(&actions, stderrPipe.writeFd);

  const std::string widthStr = std::to_string(width);
  const std::string heightStr = std::to_string(height);
  std::array<char*, 4> argv = {
      const_cast<char*>(childBinaryPath_.c_str()),
      const_cast<char*>(widthStr.c_str()),
      const_cast<char*>(heightStr.c_str()),
      nullptr,
  };

  // Curated environment: the child gets ONLY the sandbox marker plus a few
  // pre-allocated slots for debugging overrides. We deliberately drop
  // LD_PRELOAD, LD_LIBRARY_PATH, HOME, PWD, and anything else the host may
  // have accumulated. The child's `ApplyHardening()` verifies the marker
  // before reading any untrusted input.
  std::string sandboxVar = "DONNER_SANDBOX=1";
  std::array<char*, 2> childEnv = {
      sandboxVar.data(),
      nullptr,
  };

  pid_t childPid = -1;
  const int spawnErr = ::posix_spawn(&childPid, childBinaryPath_.c_str(),
                                     &actions, /*attrp=*/nullptr, argv.data(),
                                     childEnv.data());
  posix_spawn_file_actions_destroy(&actions);

  stdinPipe.closeRead();
  stdoutPipe.closeWrite();
  stderrPipe.closeWrite();

  if (spawnErr != 0) {
    stdinPipe.closeBoth();
    stdoutPipe.closeBoth();
    stderrPipe.closeBoth();
    info.status = SandboxStatus::kSpawnFailed;
    info.diagnostics = std::string("posix_spawn failed: ") + std::strerror(spawnErr);
    return info;
  }

  std::atomic<bool> writeOk{true};
  std::thread writer([&writeOk, inFd = stdinPipe.writeFd,
                      payload = std::string(svgBytes)]() mutable {
    if (!WriteAll(inFd, payload)) {
      writeOk.store(false, std::memory_order_relaxed);
    }
    ::close(inFd);
  });
  stdinPipe.writeFd = -1;

  std::string errBuf;
  std::array<pollfd, 2> fds = {
      pollfd{stdoutPipe.readFd, POLLIN, 0},
      pollfd{stderrPipe.readFd, POLLIN, 0},
  };
  bool outOpen = true;
  bool errOpen = true;
  bool readFailed = false;

  while (outOpen || errOpen) {
    fds[0].revents = 0;
    fds[1].revents = 0;
    fds[0].events = outOpen ? POLLIN : 0;
    fds[1].events = errOpen ? POLLIN : 0;

    const int n = ::poll(fds.data(), fds.size(), /*timeout=*/-1);
    if (n < 0) {
      if (errno == EINTR) continue;
      readFailed = true;
      break;
    }

    auto drainTo = [](int fd, auto appender) -> bool {
      std::array<uint8_t, 4096> buf;
      const ssize_t r = ::read(fd, buf.data(), buf.size());
      if (r > 0) {
        appender(buf.data(), static_cast<std::size_t>(r));
        return true;
      }
      if (r < 0 && errno == EINTR) return true;
      return false;
    };

    if (outOpen && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      const bool stillOpen = drainTo(stdoutPipe.readFd, [&](const uint8_t* p, std::size_t len) {
        outStdout.insert(outStdout.end(), p, p + len);
      });
      if (!stillOpen) outOpen = false;
    }
    if (errOpen && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      const bool stillOpen = drainTo(stderrPipe.readFd, [&](const uint8_t* p, std::size_t len) {
        errBuf.append(reinterpret_cast<const char*>(p), len);
      });
      if (!stillOpen) errOpen = false;
    }
  }

  writer.join();
  stdoutPipe.closeRead();
  stderrPipe.closeRead();

  int rawStatus = 0;
  while (::waitpid(childPid, &rawStatus, 0) < 0) {
    if (errno == EINTR) continue;
    info.status = SandboxStatus::kReadFailed;
    info.diagnostics = std::string("waitpid failed: ") + std::strerror(errno);
    return info;
  }

  info.status = ClassifyExit(rawStatus, info.exitCode);
  info.diagnostics = std::move(errBuf);

  if (readFailed) {
    info.status = SandboxStatus::kReadFailed;
  } else if (!writeOk.load(std::memory_order_relaxed) &&
             info.status == SandboxStatus::kOk) {
    info.status = SandboxStatus::kWriteFailed;
  }
  return info;
}

RenderResult SandboxHost::renderToBackend(std::string_view svgBytes, int width,
                                          int height, svg::RendererInterface& target) {
  RenderResult result;

  RawExitInfo info = spawnAndCollect(svgBytes, width, height, result.wire);
  result.status = info.status;
  result.exitCode = info.exitCode;
  result.diagnostics = std::move(info.diagnostics);

  if (result.status != SandboxStatus::kOk) {
    return result;
  }

  ReplayingRenderer replay(target);
  ReplayReport report;
  const ReplayStatus replayStatus = replay.pumpFrame(result.wire, report);
  result.unsupportedCount = report.unsupportedCount;

  switch (replayStatus) {
    case ReplayStatus::kOk:
      result.status = SandboxStatus::kOk;
      break;
    case ReplayStatus::kEncounteredUnsupported:
      // Still a successful render, just lossy. Keep kOk; callers inspect
      // `unsupportedCount` to decide whether to show a warning.
      result.status = SandboxStatus::kOk;
      break;
    case ReplayStatus::kHeaderMismatch:
    case ReplayStatus::kMalformed:
    case ReplayStatus::kEndOfStream:
    case ReplayStatus::kUnknownOpcode:
      result.status = SandboxStatus::kWireMalformed;
      break;
  }
  return result;
}

RenderResult SandboxHost::render(std::string_view svgBytes, int width, int height) {
  svg::Renderer backend;
  RenderResult result = renderToBackend(svgBytes, width, height, backend);
  if (result.status != SandboxStatus::kOk) {
    return result;
  }

  const svg::RendererBitmap bitmap = backend.takeSnapshot();
  if (bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0 || bitmap.pixels.empty()) {
    result.status = SandboxStatus::kRenderError;
    result.diagnostics += "\nhost-side renderer produced an empty snapshot";
    return result;
  }

  result.png = svg::RendererImageIO::writeRgbaPixelsToPngMemory(
      bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4);
  if (result.png.empty()) {
    result.status = SandboxStatus::kRenderError;
    result.diagnostics += "\nhost-side PNG encode failed";
  }
  return result;
}

}  // namespace donner::editor::sandbox
