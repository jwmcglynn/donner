#pragma once
/// @file

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/editor/ipc/spike/teleport_codec.h"
#include "donner/editor/ipc/teleport/transport.h"

extern "C" char** environ;  // POSIX, not declared by glibc's <unistd.h>.

namespace donner::teleport {

namespace detail {

struct Pipe {
  int readFd = -1;
  int writeFd = -1;

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

inline void IgnoreSigpipeOnce() {
  static const bool kInstalled = [] {
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    (void)::sigaction(SIGPIPE, &sa, nullptr);
    return true;
  }();
  (void)kInstalled;
}

inline Pipe MakePipe() {
  int fds[2] = {-1, -1};
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    return Pipe{};
  }

  return Pipe{fds[0], fds[1]};
}

inline std::string DecodeErrorMessage(DecodeError error) {
  switch (error) {
    case DecodeError::kTruncated: return "kTruncated";
    case DecodeError::kStringTooLarge: return "kStringTooLarge";
  }

  return "DecodeError(" + std::to_string(static_cast<int>(error)) + ")";
}

}  // namespace detail

/**
 * Minimal synchronous Teleport client backed by a child subprocess whose stdin
 * and stdout carry length-prefixed request/response frames.
 */
class TeleportClient {
public:
  /**
   * Spawns the child process identified by `argv[0]`.
   *
   * @param argv Program path plus any argv entries to pass to `posix_spawn`.
   */
  explicit TeleportClient(const std::vector<std::string>& argv) { spawnChild(argv); }

  ~TeleportClient() { shutdown(); }

  TeleportClient(const TeleportClient&) = delete;
  TeleportClient& operator=(const TeleportClient&) = delete;
  TeleportClient(TeleportClient&&) = delete;
  TeleportClient& operator=(TeleportClient&&) = delete;

  /// Returns whether the client successfully spawned its child process.
  [[nodiscard]] bool isReady() const {
    return childPid_ > 0 && childStdinFd_ >= 0 && childStdoutFd_ >= 0;
  }

  /// Returns the last spawn or decode failure detail captured by the client.
  [[nodiscard]] std::string_view statusMessage() const { return statusMessage_; }

  /**
   * Issues one synchronous Teleport RPC.
   *
   * @param request Request payload to encode and send to the child.
   * @tparam Request Request message type encoded with \ref Encode.
   * @tparam Response Response message type decoded with \ref Decode.
   * @return Decoded response or a \ref TransportError on transport failure.
   */
  template <class Request, class Response>
  [[nodiscard]] std::expected<Response, TransportError> call(const Request& request) {
    if (!isReady()) {
      if (statusMessage_.empty()) {
        statusMessage_ = "TeleportClient is not connected";
      }
      return std::unexpected(TransportError::kShortWrite);
    }

    statusMessage_.clear();
    const auto encodedRequest = Encode<Request>(request);
    auto writeResult = writer_.writeFrame(childStdinFd_, encodedRequest);
    if (!writeResult) {
      return std::unexpected(writeResult.error());
    }

    auto responseFrame = reader_.readFrame(childStdoutFd_);
    if (!responseFrame) {
      return std::unexpected(responseFrame.error());
    }

    auto response = Decode<Response>(*responseFrame);
    if (!response) {
      statusMessage_ = "response decode failed: " + detail::DecodeErrorMessage(response.error());
      return std::unexpected(TransportError::kShortRead);
    }

    return response.value();
  }

private:
  void spawnChild(const std::vector<std::string>& argv) {
    detail::IgnoreSigpipeOnce();

    if (argv.empty() || argv[0].empty()) {
      statusMessage_ = "argv[0] must name the child binary";
      return;
    }

    detail::Pipe stdinPipe = detail::MakePipe();
    const int stdinErrno = errno;
    detail::Pipe stdoutPipe = detail::MakePipe();
    const int stdoutErrno = errno;
    if (stdinPipe.readFd < 0 || stdinPipe.writeFd < 0) {
      stdinPipe.closeBoth();
      stdoutPipe.closeBoth();
      statusMessage_ = "pipe2(stdin) failed: " + std::string(std::strerror(stdinErrno));
      return;
    }
    if (stdoutPipe.readFd < 0 || stdoutPipe.writeFd < 0) {
      stdinPipe.closeBoth();
      stdoutPipe.closeBoth();
      statusMessage_ = "pipe2(stdout) failed: " + std::string(std::strerror(stdoutErrno));
      return;
    }

    posix_spawn_file_actions_t actions;
    const int initResult = ::posix_spawn_file_actions_init(&actions);
    if (initResult != 0) {
      stdinPipe.closeBoth();
      stdoutPipe.closeBoth();
      statusMessage_ =
          "posix_spawn_file_actions_init failed: " + std::string(std::strerror(initResult));
      return;
    }

    const auto addAction = [&](int result) -> bool {
      if (result == 0) {
        return true;
      }

      statusMessage_ = "posix_spawn_file_actions failed: " + std::string(std::strerror(result));
      return false;
    };

    const bool actionsOk =
        addAction(::posix_spawn_file_actions_adddup2(&actions, stdinPipe.readFd, STDIN_FILENO)) &&
        addAction(
            ::posix_spawn_file_actions_adddup2(&actions, stdoutPipe.writeFd, STDOUT_FILENO)) &&
        addAction(::posix_spawn_file_actions_addclose(&actions, stdinPipe.readFd)) &&
        addAction(::posix_spawn_file_actions_addclose(&actions, stdinPipe.writeFd)) &&
        addAction(::posix_spawn_file_actions_addclose(&actions, stdoutPipe.readFd)) &&
        addAction(::posix_spawn_file_actions_addclose(&actions, stdoutPipe.writeFd));

    if (!actionsOk) {
      ::posix_spawn_file_actions_destroy(&actions);
      stdinPipe.closeBoth();
      stdoutPipe.closeBoth();
      return;
    }

    std::vector<char*> childArgv;
    childArgv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      childArgv.push_back(const_cast<char*>(arg.c_str()));
    }
    childArgv.push_back(nullptr);

    pid_t pid = -1;
    const int spawnResult =
        ::posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, childArgv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);

    stdinPipe.closeRead();
    stdoutPipe.closeWrite();

    if (spawnResult != 0) {
      stdinPipe.closeBoth();
      stdoutPipe.closeBoth();
      statusMessage_ = "posix_spawn failed: " + std::string(std::strerror(spawnResult));
      return;
    }

    childPid_ = pid;
    childStdinFd_ = stdinPipe.writeFd;
    childStdoutFd_ = stdoutPipe.readFd;
    stdinPipe.writeFd = -1;
    stdoutPipe.readFd = -1;
    statusMessage_.clear();
  }

  void shutdown() {
    if (childStdinFd_ >= 0) {
      ::close(childStdinFd_);
      childStdinFd_ = -1;
    }

    if (childPid_ <= 0) {
      if (childStdoutFd_ >= 0) {
        ::close(childStdoutFd_);
        childStdoutFd_ = -1;
      }
      return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int rawStatus = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t waitResult = ::waitpid(childPid_, &rawStatus, WNOHANG);
      if (waitResult == childPid_) {
        childPid_ = -1;
        if (childStdoutFd_ >= 0) {
          ::close(childStdoutFd_);
          childStdoutFd_ = -1;
        }
        return;
      }
      if (waitResult < 0) {
        if (errno == EINTR) {
          continue;
        }

        childPid_ = -1;
        if (childStdoutFd_ >= 0) {
          ::close(childStdoutFd_);
          childStdoutFd_ = -1;
        }
        return;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void)::kill(childPid_, SIGKILL);
    while (::waitpid(childPid_, &rawStatus, 0) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    childPid_ = -1;
    if (childStdoutFd_ >= 0) {
      ::close(childStdoutFd_);
      childStdoutFd_ = -1;
    }
  }

  PipeReader reader_;
  PipeWriter writer_;
  pid_t childPid_ = -1;
  int childStdinFd_ = -1;
  int childStdoutFd_ = -1;
  std::string statusMessage_;
};

}  // namespace donner::teleport
