#include "donner/editor/sandbox/SandboxSession.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>  // IWYU pragma: keep
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/editor/sandbox/SandboxHost.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {

namespace {

void IgnoreSigpipeOnce() {
  static const bool kInstalled = [] {
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    (void)::sigaction(SIGPIPE, &sa, nullptr);
    return true;
  }();
  (void)kInstalled;
}

SandboxStatus ClassifyExit(int rawStatus, int& outExit) {
  if (WIFEXITED(rawStatus)) {
    outExit = WEXITSTATUS(rawStatus);
    if (outExit == 0) return SandboxStatus::kOk;
    return SandboxStatus::kUnknownExit;
  }
  if (WIFSIGNALED(rawStatus)) {
    outExit = -WTERMSIG(rawStatus);
    return SandboxStatus::kCrashed;
  }
  outExit = rawStatus;
  return SandboxStatus::kUnknownExit;
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

// ---------------------------------------------------------------------------
// ChildProcess PImpl
// ---------------------------------------------------------------------------

class SandboxSession::ChildProcess {
public:
  pid_t pid = -1;
  int stdinFd = -1;
  int stdoutFd = -1;
  int stderrFd = -1;

  ~ChildProcess() { closeAll(); }

  void closeStdin() {
    if (stdinFd >= 0) {
      ::close(stdinFd);
      stdinFd = -1;
    }
  }

  void closeAll() {
    closeStdin();
    if (stdoutFd >= 0) {
      ::close(stdoutFd);
      stdoutFd = -1;
    }
    if (stderrFd >= 0) {
      ::close(stderrFd);
      stderrFd = -1;
    }
  }

  ExitInfo reap(std::size_t maxStderrTail) {
    ExitInfo info;
    info.diagnostics = drainStderr(maxStderrTail);

    if (pid > 0) {
      int rawStatus = 0;
      pid_t ret = ::waitpid(pid, &rawStatus, WNOHANG);
      if (ret == 0) {
        while (::waitpid(pid, &rawStatus, 0) < 0) {
          if (errno == EINTR) continue;
          break;
        }
      } else if (ret < 0 && errno == EINTR) {
        while (::waitpid(pid, &rawStatus, 0) < 0) {
          if (errno == EINTR) continue;
          break;
        }
      }
      int exitCode = 0;
      info.status = ClassifyExit(rawStatus, exitCode);
      info.exitCode = exitCode;
      pid = -1;
    }
    closeAll();
    return info;
  }

  std::string drainStderr(std::size_t maxBytes) {
    if (stderrFd < 0) return {};
    std::string buf;
    std::array<char, 4096> tmp;
    for (;;) {
      const ssize_t n = ::read(stderrFd, tmp.data(), tmp.size());
      if (n > 0) {
        buf.append(tmp.data(), static_cast<std::size_t>(n));
        if (buf.size() > maxBytes) {
          buf.erase(0, buf.size() - maxBytes);
        }
      } else if (n == 0) {
        break;
      } else {
        if (errno == EINTR) continue;
        break;  // EAGAIN for non-blocking, or error.
      }
    }
    return buf;
  }
};

// ---------------------------------------------------------------------------
// SandboxSession
// ---------------------------------------------------------------------------

SandboxSession::SandboxSession(SandboxSessionOptions options) : options_(std::move(options)) {
  IgnoreSigpipeOnce();

  if (!spawnChild()) {
    return;
  }

  writerThread_ = std::thread([this] { writerMain(); });
  readerThread_ = std::thread([this] { readerMain(); });
}

SandboxSession::~SandboxSession() {
  {
    std::lock_guard lk(inboxMutex_);
    shutdown_ = true;
  }
  inboxCv_.notify_one();

  if (child_) {
    child_->closeStdin();
  }

  if (writerThread_.joinable()) writerThread_.join();
  if (readerThread_.joinable()) readerThread_.join();

  if (child_ && child_->pid > 0) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    for (;;) {
      int status = 0;
      pid_t ret = ::waitpid(child_->pid, &status, WNOHANG);
      if (ret != 0) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        ::kill(child_->pid, SIGKILL);
        while (::waitpid(child_->pid, &status, 0) < 0) {
          if (errno == EINTR) continue;
          break;
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    child_->pid = -1;
  }
  child_.reset();
}

std::future<WireResponse> SandboxSession::submit(WireRequest request) {
  // Extract the requestId from the pre-encoded session frame bytes.
  // Layout: magic(4) + requestId(8) + opcode(4) + length(4) + payload...
  uint64_t id = 0;
  if (request.bytes.size() >= 12) {
    std::memcpy(&id, request.bytes.data() + 4, sizeof(id));
  } else {
    // Malformed frame — use an internal counter as fallback.
    id = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
  }

  Pending p;
  p.requestId = id;
  p.bytes = std::move(request.bytes);
  auto future = p.promise.get_future();

  {
    std::lock_guard lk(inboxMutex_);
    inbox_.push_back(std::move(p));
  }
  inboxCv_.notify_one();
  return future;
}

bool SandboxSession::childAlive() const {
  return childAlive_.load(std::memory_order_relaxed);
}

std::optional<ExitInfo> SandboxSession::lastExit() const {
  std::lock_guard lk(diagnosticsMutex_);
  return lastExit_;
}

void SandboxSession::setDiagnosticCallback(DiagnosticCallback cb) {
  std::lock_guard lk(diagnosticsMutex_);
  diagnosticCallback_ = std::move(cb);
}

bool SandboxSession::spawnChild() {
  Pipe stdinPipe = MakePipe();
  Pipe stdoutPipe = MakePipe();
  Pipe stderrPipe = MakePipe();
  if (!stdinPipe.valid() || !stdoutPipe.valid() || !stderrPipe.valid()) {
    stdinPipe.closeBoth();
    stdoutPipe.closeBoth();
    stderrPipe.closeBoth();
    return false;
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

  std::array<char*, 2> argv = {
      const_cast<char*>(options_.childBinaryPath.c_str()),
      nullptr,
  };

  std::string sandboxVar = "DONNER_SANDBOX=1";
  std::array<char*, 2> childEnv = {
      sandboxVar.data(),
      nullptr,
  };

  pid_t childPid = -1;
  const int spawnErr = ::posix_spawn(&childPid, options_.childBinaryPath.c_str(), &actions,
                                     /*attrp=*/nullptr, argv.data(), childEnv.data());
  posix_spawn_file_actions_destroy(&actions);

  stdinPipe.closeRead();
  stdoutPipe.closeWrite();
  stderrPipe.closeWrite();

  if (spawnErr != 0) {
    stdinPipe.closeBoth();
    stdoutPipe.closeBoth();
    stderrPipe.closeBoth();
    return false;
  }

  // Make stderr non-blocking for drain.
  int flags = ::fcntl(stderrPipe.readFd, F_GETFL);
  if (flags >= 0) {
    ::fcntl(stderrPipe.readFd, F_SETFL, flags | O_NONBLOCK);
  }

  child_ = std::make_unique<ChildProcess>();
  child_->pid = childPid;
  child_->stdinFd = stdinPipe.writeFd;
  child_->stdoutFd = stdoutPipe.readFd;
  child_->stderrFd = stderrPipe.readFd;
  childAlive_.store(true, std::memory_order_relaxed);

  return true;
}

void SandboxSession::writerMain() {
  for (;;) {
    Pending pending;
    {
      std::unique_lock lk(inboxMutex_);
      inboxCv_.wait(lk, [this] { return shutdown_ || !inbox_.empty(); });

      if (shutdown_ && inbox_.empty()) return;
      if (inbox_.empty()) continue;

      pending = std::move(inbox_.front());
      inbox_.pop_front();
    }

    // Move promise into awaiting_ so the reader can fulfill it.
    {
      std::lock_guard lk(awaitingMutex_);
      Pending awaitEntry;
      awaitEntry.requestId = pending.requestId;
      awaitEntry.promise = std::move(pending.promise);
      awaiting_.push_back(std::move(awaitEntry));
    }

    // Write the pre-encoded frame to child stdin.
    if (!child_ || child_->stdinFd < 0) {
      continue;
    }

    const uint8_t* data = pending.bytes.data();
    std::size_t remaining = pending.bytes.size();
    while (remaining > 0) {
      const ssize_t n = ::write(child_->stdinFd, data, remaining);
      if (n > 0) {
        data += n;
        remaining -= static_cast<std::size_t>(n);
      } else if (n < 0) {
        if (errno == EINTR) continue;
        break;  // EPIPE — child died, readerMain handles.
      }
    }
  }
}

void SandboxSession::readerMain() {
  std::vector<uint8_t> readBuf;
  readBuf.reserve(64 * 1024);

  auto dispatchFrame = [this](const SessionFrame& frame) {
    switch (frame.opcode) {
      case SessionOpcode::kFrame:
      case SessionOpcode::kHandshakeAck:
      case SessionOpcode::kShutdownAck:
      case SessionOpcode::kExportResponse:
      case SessionOpcode::kSourceReplaceAll:
      case SessionOpcode::kError: {
        // Resolve the matching awaiting promise.
        std::lock_guard lk(awaitingMutex_);
        for (auto it = awaiting_.begin(); it != awaiting_.end(); ++it) {
          if (it->requestId == frame.requestId) {
            WireResponse resp;
            resp.status = SandboxStatus::kOk;
            resp.bytes = frame.payload;
            it->promise.set_value(std::move(resp));
            awaiting_.erase(it);
            return;
          }
        }
        break;
      }

      case SessionOpcode::kDiagnostic: {
        std::string msg(frame.payload.begin(), frame.payload.end());
        std::lock_guard lk(diagnosticsMutex_);
        if (diagnosticCallback_) {
          diagnosticCallback_(msg);
        }
        break;
      }

      case SessionOpcode::kToast:
      case SessionOpcode::kDialogRequest: {
        std::string msg = "[push:";
        msg += std::to_string(static_cast<uint32_t>(frame.opcode));
        msg += "] ";
        msg.append(frame.payload.begin(), frame.payload.end());
        std::lock_guard lk(diagnosticsMutex_);
        if (diagnosticCallback_) {
          diagnosticCallback_(msg);
        }
        break;
      }

      default: {
        std::string msg = "unknown session opcode from child: ";
        msg += std::to_string(static_cast<uint32_t>(frame.opcode));
        std::lock_guard lk(diagnosticsMutex_);
        if (diagnosticCallback_) {
          diagnosticCallback_(msg);
        }
        break;
      }
    }
  };

  auto handleChildDeath = [this]() {
    childAlive_.store(false, std::memory_order_relaxed);

    ExitInfo info;
    if (child_) {
      info = child_->reap(options_.maxStderrTailBytes);
      child_.reset();
    } else {
      info.status = SandboxStatus::kCrashed;
      info.diagnostics = "child process lost";
    }

    {
      std::lock_guard lk(diagnosticsMutex_);
      lastExit_ = info;
    }

    failInFlight(info.status, info.diagnostics);

    if (options_.autoRespawn && !shutdown_) {
      spawnChild();
    }
  };

  for (;;) {
    if (shutdown_) return;
    if (!child_ || child_->stdoutFd < 0) {
      // No child — wait briefly in case respawn is happening.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (shutdown_) return;
      continue;
    }

    std::array<uint8_t, 8192> tmp;
    const ssize_t n = ::read(child_->stdoutFd, tmp.data(), tmp.size());

    if (n > 0) {
      readBuf.insert(readBuf.end(), tmp.data(), tmp.data() + n);

      for (;;) {
        SessionFrame frame;
        std::size_t consumed = 0;
        if (!DecodeFrame(std::span<const uint8_t>(readBuf), frame, consumed)) {
          if (consumed == SIZE_MAX) {
            handleChildDeath();
            readBuf.clear();
            break;
          }
          break;  // Need more data.
        }
        readBuf.erase(readBuf.begin(), readBuf.begin() + static_cast<ptrdiff_t>(consumed));
        dispatchFrame(frame);
      }
    } else if (n == 0) {
      // EOF.
      handleChildDeath();
      if (shutdown_) return;
      readBuf.clear();
    } else {
      if (errno == EINTR) continue;
      handleChildDeath();
      if (shutdown_) return;
      readBuf.clear();
    }
  }
}

void SandboxSession::failInFlight(SandboxStatus status, const std::string& diagnostics) {
  std::lock_guard lk(awaitingMutex_);
  for (auto& p : awaiting_) {
    WireResponse resp;
    resp.status = status;
    resp.diagnostics = diagnostics;
    p.promise.set_value(std::move(resp));
  }
  awaiting_.clear();
}

}  // namespace donner::editor::sandbox
