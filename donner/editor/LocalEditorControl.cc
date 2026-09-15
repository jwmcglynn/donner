#include "donner/editor/LocalEditorControl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace donner::editor {
using Json = nlohmann::json;
namespace {
constexpr std::size_t kMaximumRequestBytes = 1024 * 1024;
constexpr std::size_t kMaximumResponseBytes = 16 * 1024 * 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(30);
Json RpcError(const Json& id, std::string message, int code = -32000) {
  return {
      {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(message)}}}};
}
bool BoundedJsonDepth(std::string_view bytes) {
  int depth = 0;
  bool quoted = false, escaped = false;
  for (char c : bytes) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '\"')
        quoted = false;
    } else if (c == '\"')
      quoted = true;
    else if ((c == '{' || c == '[') && ++depth > 64)
      return false;
    else if (c == '}' || c == ']')
      --depth;
  }
  return true;
}
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
bool ValidSocketAddress(const std::string& path) {
  return std::filesystem::path(path).is_absolute() && path.size() < sizeof(sockaddr_un::sun_path) &&
         path.find('\0') == std::string::npos;
}
bool PrivateSocketDirectory(const std::string& path) {
  const std::string parent = std::filesystem::path(path).parent_path().string();
  struct stat directory{};
  return lstat(parent.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
         directory.st_uid == geteuid() && (directory.st_mode & 0077) == 0;
}
bool SameUserPeer(int fd) {
#if defined(__APPLE__)
  uid_t uid = 0;
  gid_t gid = 0;
  return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#elif defined(__linux__)
  ucred credentials{};
  socklen_t length = sizeof(credentials);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
         credentials.uid == geteuid();
#else
  return false;
#endif
}

bool ConfigureNonblocking(std::initializer_list<int> descriptors) {
  for (int fd : descriptors) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fcntl(fd, F_SETFL, O_NONBLOCK) != 0) return false;
  }
  return true;
}

int BindPrivateSocket(const std::string& path, struct stat* identity, std::string* error) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = "Cannot create control socket";
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    *error = "Cannot bind control socket";
    close(fd);
    return -1;
  }
  if (chmod(path.c_str(), 0600) != 0 || listen(fd, 8) != 0 || lstat(path.c_str(), identity) != 0) {
    *error = "Cannot secure or listen on control socket";
    close(fd);
    unlink(path.c_str());
    return -1;
  }
  return fd;
}
int OpenPrivateFeedback(const std::string& path, std::size_t* size) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  struct stat info{};
  const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                     (info.st_mode & 0077) == 0 && info.st_size >= 0 &&
                     info.st_size <= 4 * 1024 * 1024;
  if (!valid) {
    close(fd);
    return -1;
  }
  *size = static_cast<std::size_t>(info.st_size);
  return fd;
}
std::optional<std::string> ReadFeedbackBytes(int fd, std::size_t size) {
  std::string bytes(size, '\0');
  std::size_t received = 0;
  while (received < bytes.size()) {
    const ssize_t count = read(fd, bytes.data() + received, bytes.size() - received);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return std::nullopt;
    received += static_cast<std::size_t>(count);
  }
  return bytes;
}
bool WriteFeedbackBytes(int fd, const std::string& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    written += static_cast<std::size_t>(count);
  }
  return fsync(fd) == 0;
}

struct ClientSocket {
  int fd = -1;
  ~ClientSocket() {
    if (fd >= 0) close(fd);
  }
};

using SocketDeadline = std::chrono::steady_clock::time_point;

bool WaitSocket(int fd, short events, SocketDeadline deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd descriptor{fd, events, 0};
    const int ready = poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
    if (ready < 0 && errno == EINTR) continue;
    return ready > 0 && (descriptor.revents & events) != 0;
  }
  return false;
}

std::unique_ptr<ClientSocket> ConnectEditor(const std::string& path, std::string* error) {
  struct stat endpoint{};
  if (!ValidSocketAddress(path) || !PrivateSocketDirectory(path) ||
      lstat(path.c_str(), &endpoint) != 0 || !S_ISSOCK(endpoint.st_mode) ||
      endpoint.st_uid != geteuid() || (endpoint.st_mode & 0077) != 0) {
    *error = "Editor endpoint and directory must be private and owned by the current user";
    return nullptr;
  }
  auto connection = std::make_unique<ClientSocket>();
  connection->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (connection->fd < 0 || fcntl(connection->fd, F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(connection->fd, F_SETFL, O_NONBLOCK) != 0) {
    *error = "Cannot create editor connection";
    return nullptr;
  }
#ifdef SO_NOSIGPIPE
  int noSignal = 1;
  setsockopt(connection->fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
  const int connected =
      connect(connection->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  int socketError = 0;
  socklen_t length = sizeof(socketError);
  if ((connected != 0 && errno != EINPROGRESS && errno != EINTR) ||
      !WaitSocket(connection->fd, POLLOUT, deadline) ||
      getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 ||
      socketError != 0 || !SameUserPeer(connection->fd)) {
    *error = "Cannot connect to the private editor endpoint";
    return nullptr;
  }
  return connection;
}

class NonblockingDescriptor {
public:
  explicit NonblockingDescriptor(int fd) : fd_(fd), previous_(fcntl(fd, F_GETFL)) {
    valid_ = previous_ >= 0 && fcntl(fd, F_SETFL, previous_ | O_NONBLOCK) == 0;
  }
  ~NonblockingDescriptor() {
    if (valid_) (void)fcntl(fd_, F_SETFL, previous_);
  }
  bool valid() const { return valid_; }

private:
  int fd_;
  int previous_;
  bool valid_ = false;
};

class BlockBrokenPipe {
public:
  BlockBrokenPipe() {
    sigemptyset(&signal_);
    sigaddset(&signal_, SIGPIPE);
    valid_ = pthread_sigmask(SIG_BLOCK, &signal_, &previous_) == 0;
    sigset_t pending{};
    if (valid_ && sigpending(&pending) == 0) wasPending_ = sigismember(&pending, SIGPIPE) == 1;
  }
  ~BlockBrokenPipe() {
    if (!valid_) return;
    sigset_t pending{};
    if (!wasPending_ && sigismember(&previous_, SIGPIPE) != 1 && sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1) {
      int signal = 0;
      (void)sigwait(&signal_, &signal);
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }
  bool valid() const { return valid_; }

private:
  sigset_t signal_{};
  sigset_t previous_{};
  bool valid_ = false;
  bool wasPending_ = false;
};

class FrameQueue {
public:
  static constexpr std::size_t kMaximumBytes = 32 * 1024 * 1024;
  static constexpr std::size_t kMaximumFrames = 64;
  bool append(std::string bytes) {
    if (frames_.size() >= kMaximumFrames || bytes.size() > kMaximumBytes - bytes_) return false;
    if (frames_.empty()) progress_ = std::chrono::steady_clock::now();
    bytes_ += bytes.size();
    frames_.push_back(std::move(bytes));
    return true;
  }
  bool flush(int fd) {
    if (frames_.empty()) return true;
    const auto& bytes = frames_.front();
    const ssize_t count = write(fd, bytes.data() + sent_, bytes.size() - sent_);
    if (count < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    if (count == 0) return false;
    sent_ += static_cast<std::size_t>(count);
    progress_ = std::chrono::steady_clock::now();
    if (sent_ == bytes.size()) {
      bytes_ -= bytes.size();
      frames_.pop_front();
      sent_ = 0;
    }
    return true;
  }
  bool hasResponseRoom() const {
    return frames_.size() < kMaximumFrames && bytes_ <= kMaximumBytes - kMaximumResponseBytes - 1;
  }
  bool stalled(SocketDeadline now) const {
    return !empty() && now - progress_ >= std::chrono::seconds(10);
  }
  bool empty() const { return frames_.empty(); }
  void clear() {
    frames_.clear();
    bytes_ = sent_ = 0;
  }

private:
  std::deque<std::string> frames_;
  std::size_t bytes_ = 0;
  std::size_t sent_ = 0;
  SocketDeadline progress_ = std::chrono::steady_clock::now();
};

class NativeStdioBridge {
public:
  NativeStdioBridge(std::string path, int input, int output, std::ostream& errors,
                    EditorControlStdioOptions options)
      : path_(std::move(path)),
        inputFd_(input),
        outputFd_(output),
        errors_(errors),
        options_(options) {}

  int run() {
    if (!ValidSocketAddress(path_) || options_.heartbeatInterval.count() <= 0 ||
        options_.heartbeatInterval > std::chrono::seconds(30) ||
        options_.requestTimeout.count() <= 0 ||
        options_.requestTimeout > std::chrono::seconds(60)) {
      errors_ << "Invalid native MCP endpoint or liveness policy\n";
      return 2;
    }
    NonblockingDescriptor inputMode(inputFd_), outputMode(outputFd_);
    BlockBrokenPipe brokenPipe;
    if (!inputMode.valid() || !outputMode.valid() || !brokenPipe.valid()) {
      errors_ << "Cannot configure native MCP descriptors\n";
      return 1;
    }
    while (true) {
      consumeInput();
      const auto now = std::chrono::steady_clock::now();
      checkDeadlines(now);
      if (stopping_ && replies_.empty()) return exitCode_;
      if (inputDrained() && replies_.empty()) return exitCode_;
      if (replies_.stalled(now)) {
        errors_ << "MCP output stopped accepting replies\n";
        return 1;
      }
      heartbeat(now);
      if (!pollOnce(now)) return 1;
    }
  }

private:
  struct PendingReply {
    Json originalId;
    SocketDeadline deadline;
    bool heartbeat;
  };
  bool inputDrained() const {
    return inputEnded_ && input_.empty() && pending_.empty() && notificationDeadlines_.empty() &&
           commands_.empty();
  }
  bool canAccept() const {
    return !stopping_ && pending_.size() + notificationDeadlines_.size() < 8;
  }
  bool queueReply(const Json& reply) {
    return replies_.append(reply.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n");
  }
  void stop(std::string reason, int code = 1, const Json& extraId = Json(nullptr)) {
    if (stopping_) return;
    errors_ << reason << '\n';
    for (const auto& [id, pending] : pending_) {
      if (!pending.heartbeat) (void)queueReply(RpcError(pending.originalId, reason));
    }
    if (!extraId.is_null() || code == 2)
      (void)queueReply(RpcError(extraId, reason, code == 2 ? -32700 : -32000));
    stopping_ = inputEnded_ = true;
    exitCode_ = code;
    input_.clear();
    pending_.clear();
    notificationDeadlines_.clear();
    commands_.clear();
    connection_.reset();
  }
  bool validRequest(const Json& request) {
    return request.is_object() && request.value("jsonrpc", Json(nullptr)) == "2.0" &&
           (!request.contains("id") || request["id"].is_string() ||
            request["id"].is_number_integer());
  }
  void consumeInput() {
    while (canAccept()) {
      const auto newline = input_.find('\n');
      if (newline == std::string::npos) {
        if (input_.size() > kMaximumRequestBytes || (inputEnded_ && !input_.empty()))
          stop("MCP input must be bounded newline-delimited JSON", 2);
        return;
      }
      if (newline > kMaximumRequestBytes ||
          !BoundedJsonDepth(std::string_view(input_).substr(0, newline))) {
        stop("MCP input must be bounded newline-delimited JSON", 2);
        return;
      }
      Json request = Json::parse(
          input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(newline), nullptr, false);
      input_.erase(0, newline + 1);
      inputSince_ = std::chrono::steady_clock::now();
      if (!validRequest(request)) {
        stop("Invalid MCP JSON-RPC object, version or request ID", 2);
        return;
      }
      enqueue(std::move(request), false);
    }
  }
  bool translateCancellation(Json& request) {
    if (request.value("method", Json(nullptr)) != "notifications/cancelled") return true;
    auto params = request.find("params");
    if (params == request.end() || !params->is_object() || !params->contains("requestId"))
      return false;
    for (const auto& [id, pending] : pending_) {
      if (!pending.heartbeat && pending.originalId == (*params)["requestId"]) {
        (*params)["requestId"] = id;
        return true;
      }
    }
    return false;
  }
  void enqueue(Json request, bool heartbeat) {
    const Json originalId = request.value("id", Json(nullptr));
    if (!heartbeat && !originalId.is_null()) {
      for (const auto& [id, pending] : pending_) {
        if (!pending.heartbeat && pending.originalId == originalId) {
          stop("Duplicate pending MCP request ID", 2);
          return;
        }
      }
    }
    if (!translateCancellation(request)) return;
    if (!connection_) {
      std::string error;
      connection_ = ConnectEditor(path_, &error);
      if (!connection_) {
        stop(std::move(error), 1, originalId);
        return;
      }
      nextHeartbeat_ = std::chrono::steady_clock::now() + options_.heartbeatInterval;
    }
    const auto deadline = std::chrono::steady_clock::now() + options_.requestTimeout;
    if (request.contains("id")) {
      if (nextId_ == std::numeric_limits<std::uint64_t>::max()) {
        stop("Native MCP request identity space exhausted", 1, originalId);
        return;
      }
      request["id"] = nextId_;
      pending_.emplace(nextId_++, PendingReply{originalId, deadline, heartbeat});
    } else
      notificationDeadlines_.push_back(deadline);
    if (!commands_.append(request.dump() + "\n")) stop("Native MCP command queue limit exceeded");
  }
  void heartbeat(SocketDeadline now) {
    if (!connection_ || inputEnded_ || !canAccept() || now < nextHeartbeat_) return;
    nextHeartbeat_ = now + options_.heartbeatInterval;
    enqueue(Json{{"jsonrpc", "2.0"}, {"id", 0}, {"method", "ping"}}, true);
  }
  void checkDeadlines(SocketDeadline now) {
    if (stopping_) return;
    for (const auto& [id, pending] : pending_) {
      if (now >= pending.deadline) {
        stop("Editor request timed out; reread state before retrying");
        return;
      }
    }
    if (!notificationDeadlines_.empty() && now >= notificationDeadlines_.front()) {
      stop("Editor notification acknowledgement timed out");
      return;
    }
    if ((!input_.empty() && input_.find('\n') == std::string::npos &&
         now - inputSince_ >= std::chrono::seconds(10)) ||
        commands_.stalled(now))
      stop("Native MCP framing or write deadline exceeded");
  }
  void routeResponse(Json response) {
    if (response.is_null()) {
      if (notificationDeadlines_.empty())
        stop("Unexpected editor notification acknowledgement");
      else
        notificationDeadlines_.pop_front();
      return;
    }
    if (!response.is_object() || response.value("jsonrpc", Json(nullptr)) != "2.0") {
      stop("Invalid editor response framing");
      return;
    }
    if (!response.contains("id")) {
      const auto method = response.find("method");
      if (method == response.end() || !method->is_string() ||
          !method->get<std::string>().starts_with("notifications/") || !queueReply(response))
        stop("Invalid or excessive editor notification");
      return;
    }
    if (response["id"].is_null() && !notificationDeadlines_.empty()) {
      notificationDeadlines_.pop_front();
      return;
    }
    if (!response["id"].is_number_unsigned()) {
      stop("Editor response ID is invalid");
      return;
    }
    auto found = pending_.find(response["id"].get<std::uint64_t>());
    if (found == pending_.end()) {
      stop("Editor response ID does not match a pending request");
      return;
    }
    const PendingReply pending = found->second;
    pending_.erase(found);
    if (pending.heartbeat) {
      if (!response.contains("result") || !response["result"].is_object() ||
          !response["result"].empty())
        stop("Editor heartbeat was rejected");
      return;
    }
    response["id"] = pending.originalId;
    if (!queueReply(response)) stop("Native MCP reply queue limit exceeded", 1, pending.originalId);
  }
  void consumeResponses() {
    while (!stopping_ && replies_.hasResponseRoom()) {
      const auto newline = response_.find('\n');
      if (newline == std::string::npos) {
        if (response_.size() > kMaximumResponseBytes) stop("Editor response exceeds 16 MiB");
        return;
      }
      if (newline > kMaximumResponseBytes ||
          !BoundedJsonDepth(std::string_view(response_).substr(0, newline))) {
        stop("Invalid editor response framing or nesting");
        return;
      }
      Json parsed =
          Json::parse(response_.begin(), response_.begin() + static_cast<std::ptrdiff_t>(newline),
                      nullptr, false);
      response_.erase(0, newline + 1);
      routeResponse(std::move(parsed));
    }
  }
  bool readInput() {
    std::array<char, 8192> bytes{};
    const ssize_t count = read(inputFd_, bytes.data(),
                               std::min(bytes.size(), kMaximumRequestBytes + 1 - input_.size()));
    if (count < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    if (count == 0) {
      inputEnded_ = true;
      return true;
    }
    if (input_.empty()) inputSince_ = std::chrono::steady_clock::now();
    input_.append(bytes.data(), static_cast<std::size_t>(count));
    return true;
  }
  void readResponses() {
    std::array<char, 8192> bytes{};
    const ssize_t count =
        read(connection_->fd, bytes.data(),
             std::min(bytes.size(), kMaximumResponseBytes + 1 - response_.size()));
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (count == 0 && inputDrained() && response_.empty()) {
      connection_.reset();
      return;
    }
    if (count <= 0) {
      stop("Editor connection ended; reread state before retrying edits");
      return;
    }
    response_.append(bytes.data(), static_cast<std::size_t>(count));
    consumeResponses();
  }
  bool pollOnce(SocketDeadline now) {
    consumeResponses();
    short remoteEvents = replies_.hasResponseRoom() ? POLLIN : 0;
    if (!commands_.empty()) remoteEvents |= POLLOUT;
    std::array<pollfd, 3> fds{{{!inputEnded_ && canAccept() ? inputFd_ : -1, POLLIN, 0},
                               {connection_ ? connection_->fd : -1, remoteEvents, 0},
                               {!replies_.empty() ? outputFd_ : -1, POLLOUT, 0}}};
    int timeoutMs = 1000;
    if (connection_ && !inputEnded_)
      timeoutMs = std::clamp(
          static_cast<int>(
              std::chrono::duration_cast<std::chrono::milliseconds>(nextHeartbeat_ - now).count()),
          1, 1000);
    const int result = poll(fds.data(), fds.size(), timeoutMs);
    if (result < 0) return errno == EINTR;
    if (fds[2].revents && !replies_.flush(outputFd_)) return false;
    if (fds[0].revents && !readInput()) stop("MCP input descriptor failed");
    if (connection_ && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) readResponses();
    if (connection_ && (fds[1].revents & POLLOUT) && !commands_.flush(connection_->fd))
      stop("Cannot write to editor connection");
    return true;
  }
  std::string path_;
  int inputFd_;
  int outputFd_;
  std::ostream& errors_;
  EditorControlStdioOptions options_;
  std::unique_ptr<ClientSocket> connection_;
  std::map<std::uint64_t, PendingReply> pending_;
  std::deque<SocketDeadline> notificationDeadlines_;
  FrameQueue commands_, replies_;
  std::string input_, response_;
  SocketDeadline inputSince_ = std::chrono::steady_clock::now();
  SocketDeadline nextHeartbeat_ = inputSince_;
  std::uint64_t nextId_ = 1;
  bool inputEnded_ = false;
  bool stopping_ = false;
  int exitCode_ = 0;
};

#endif
}  // namespace

struct LocalEditorControl::Impl {
  struct Pending {
    Json request;
    AgentSession agent;
    std::string response;
    std::chrono::steady_clock::time_point deadline;
    bool complete = false;
    bool cancelled = false;
    bool overloaded = false;
  };
  std::string path;
  std::function<void()> wake;
  std::thread worker;
  mutable std::mutex mutex;
  std::deque<std::shared_ptr<Pending>> pending;
  std::size_t outstandingRequests = 0;
  std::size_t responseBytes = 0;
  bool stopping = false;
  bool presenceChanged = false;
  std::uint64_t nextConnectionId = 1;
  std::chrono::milliseconds idleTimeout{60000};
  int listener = -1;
  std::array<int, 2> stopPipe{-1, -1};
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  static constexpr std::size_t kMaximumClients = 8;
  static constexpr std::size_t kMaximumClientMessages = 8;
  static constexpr std::size_t kMaximumPendingRequests = 32;
  static constexpr std::size_t kMaximumQueuedResponseBytes = 32 * 1024 * 1024;
  static constexpr auto kIoTimeout = std::chrono::seconds(10);
  enum class Phase { New, Initializing, Connected };
  struct Client {
    int fd;
    AgentSession agent;
    Phase phase = Phase::New;
    std::string input;
    std::deque<std::shared_ptr<Pending>> requests;
    std::deque<std::string> output;
    std::size_t sent = 0;
    SocketDeadline inputSince = std::chrono::steady_clock::now();
    SocketDeadline outputProgress = inputSince;
    SocketDeadline activity = inputSince;
    bool closeAfterOutput = false;
  };
  std::vector<Client> clients;
  dev_t device = 0;
  ino_t inode = 0;

  // Called under mutex; a full nonblocking pipe already carries a wakeup.
  void wakeIo() {
    if (stopPipe[1] >= 0) {
      const char signal = 1;
      (void)write(stopPipe[1], &signal, 1);
    }
  }
  bool validLease() const {
    return idleTimeout.count() > 0 && idleTimeout <= std::chrono::seconds(60);
  }
  bool canStart(std::string* error) const {
    if (!validLease()) {
      *error = "Invalid collaboration connection lease";
      return false;
    }
    if (worker.joinable()) {
      *error = "Collaboration endpoint is already running";
      return false;
    }
    return true;
  }
  void interrupt() {
    std::lock_guard lock(mutex);
    stopping = true;
    wakeIo();
  }
  void closeDescriptors() {
    if (listener >= 0) {
      close(listener);
      listener = -1;
    }
    for (int& fd : stopPipe) {
      if (fd >= 0) close(fd);
      fd = -1;
    }
  }
  void removeOwnedSocket() {
    struct stat current{};
    if (!path.empty() && lstat(path.c_str(), &current) == 0 && current.st_dev == device &&
        current.st_ino == inode && S_ISSOCK(current.st_mode))
      unlink(path.c_str());
  }
  void closeClient(std::size_t index) {
    Client& client = clients[index];
    presenceChanged = presenceChanged || client.phase == Phase::Connected;
    for (auto& item : client.requests) {
      item->cancelled = true;
      responseBytes -= item->response.size();
      item->response.clear();
      --outstandingRequests;
    }
    for (const auto& bytes : client.output) responseBytes -= bytes.size();
    close(client.fd);
    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
    std::erase_if(pending, [](const auto& item) { return item->cancelled; });
  }
  void complete(const std::shared_ptr<Pending>& item, const Json& response) {
    std::string bytes = response.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (bytes.size() > kMaximumResponseBytes)
      bytes = RpcError(item->request.value("id", Json(nullptr)), "Response exceeds 16 MiB").dump();
    bytes += '\n';
    if (bytes.size() > kMaximumQueuedResponseBytes - responseBytes) {
      item->overloaded = true;
      item->cancelled = true;
      return;
    }
    responseBytes += bytes.size();
    item->response = std::move(bytes);
    item->complete = true;
  }
  static bool validLabel(const Json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    return !text.empty() && text.size() <= 128 && text.front() != ' ' && text.back() != ' ' &&
           std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return c >= 32 && c <= 126; });
  }
  void initialize(Client& client, const std::shared_ptr<Pending>& item) {
    const auto& request = item->request;
    const Json id = request.value("id", Json(nullptr));
    const Json params = request.value("params", Json(nullptr));
    if (client.phase != Phase::New) {
      complete(item, RpcError(id, "Agent connection is already initialized", -32600));
      return;
    }
    if (request.value("jsonrpc", Json(nullptr)) != "2.0" ||
        !(id.is_string() || id.is_number_integer()) || !params.is_object() ||
        !params.value("capabilities", Json(nullptr)).is_object() ||
        !params.value("clientInfo", Json(nullptr)).is_object()) {
      complete(item, RpcError(id, "Invalid initialization fields", -32602));
      return;
    }
    const Json info = params["clientInfo"];
    const Json version = params.value("protocolVersion", Json(nullptr));
    if (!validLabel(info.value("name", Json(nullptr))) ||
        !validLabel(info.value("version", Json(nullptr))) || !validLabel(version)) {
      complete(item, RpcError(id, "Invalid initialization identity or version", -32602));
      return;
    }
    std::string protocol = version.get<std::string>();
    if (protocol != "2024-11-05" && protocol != "2025-03-26" && protocol != "2025-06-18")
      protocol = "2025-06-18";
    client.agent.name = info["name"].get<std::string>();
    client.agent.version = info["version"].get<std::string>();
    client.phase = Phase::Initializing;
    complete(item,
             Json{{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"protocolVersion", protocol},
                    {"capabilities", {{"tools", Json::object()}}},
                    {"serverInfo", {{"name", "donner-native-editor"}, {"version", "0.2.0"}}}}}});
  }
  void cancelRequest(Client& client, const Json& request) {
    const Json params = request.value("params", Json(nullptr));
    if (!params.is_object() || !params.contains("requestId") ||
        !(params["requestId"].is_string() || params["requestId"].is_number_integer()))
      return;
    for (const auto& pending : client.requests) {
      if (!pending->complete &&
          pending->request.value("id", Json(nullptr)) == params["requestId"]) {
        pending->cancelled = true;
        complete(pending, RpcError(params["requestId"],
                                   "Request cancelled; reread state before retrying", -32800));
      }
    }
  }
  bool handleProtocol(Client& client, const std::shared_ptr<Pending>& item) {
    const auto method = item->request.value("method", Json(nullptr));
    const Json id = item->request.value("id", Json(nullptr));
    if (method == "initialize") {
      initialize(client, item);
      return true;
    }
    if (method == "ping") {
      complete(item, id.is_null()
                         ? Json(nullptr)
                         : Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}});
      return true;
    }
    if (method == "notifications/initialized" && id.is_null()) {
      if (client.phase == Phase::Initializing) {
        client.phase = Phase::Connected;
        presenceChanged = true;
      } else if (client.phase == Phase::New)
        client.closeAfterOutput = true;
      complete(item, Json(nullptr));
      return true;
    }
    if (client.phase != Phase::Connected) {
      complete(item,
               id.is_null()
                   ? Json(nullptr)
                   : RpcError(id, "Initialize an agent session before editor requests", -32002));
      return true;
    }
    if (method == "notifications/cancelled" && id.is_null()) {
      cancelRequest(client, item->request);
      complete(item, Json(nullptr));
      return true;
    }
    return false;
  }
  bool canRead(const Client& client) const {
    return !client.closeAfterOutput &&
           client.requests.size() + client.output.size() < kMaximumClientMessages &&
           outstandingRequests < kMaximumPendingRequests;
  }
  bool parseBuffered(Client& client) {
    bool queued = false;
    while (canRead(client)) {
      const auto newline = client.input.find('\n');
      if (newline == std::string::npos && client.input.size() <= kMaximumRequestBytes) break;
      auto item = std::make_shared<Pending>();
      item->deadline = std::chrono::steady_clock::now() + kRequestTimeout;
      std::string reason;
      if (newline == std::string::npos || newline > kMaximumRequestBytes) {
        reason = "Request exceeds 1 MiB";
      } else {
        const std::string_view frame(client.input.data(), newline);
        if (!BoundedJsonDepth(frame))
          reason = "JSON nesting exceeds 64 levels";
        else {
          item->request = Json::parse(frame, nullptr, false);
          if (item->request.is_discarded() || !item->request.is_object())
            reason = "Invalid JSON object";
        }
      }
      ++outstandingRequests;
      client.requests.push_back(item);
      if (!reason.empty()) {
        item->request = Json::object();
        complete(item, RpcError(nullptr, std::move(reason)));
        client.input.clear();
        client.closeAfterOutput = true;
        break;
      }
      client.input.erase(0, newline + 1);
      client.inputSince = std::chrono::steady_clock::now();
      item->agent = client.agent;
      if (!handleProtocol(client, item)) {
        pending.push_back(item);
        queued = true;
      }
    }
    return queued;
  }
  bool receive(Client& client, bool* queued) {
    std::array<char, 8192> buffer{};
    const std::size_t capacity = kMaximumRequestBytes + 1 - client.input.size();
    const ssize_t count = recv(client.fd, buffer.data(), std::min(buffer.size(), capacity), 0);
    if (count < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    if (count == 0) return false;
    if (client.input.empty()) client.inputSince = std::chrono::steady_clock::now();
    client.activity = std::chrono::steady_clock::now();
    client.input.append(buffer.data(), static_cast<std::size_t>(count));
    *queued = parseBuffered(client) || *queued;
    return true;
  }
  bool sendOutput(Client& client) {
    if (client.output.empty()) return true;
    const std::string& bytes = client.output.front();
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const ssize_t count =
        send(client.fd, bytes.data() + client.sent, bytes.size() - client.sent, flags);
    if (count < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    if (count == 0) return false;
    client.sent += static_cast<std::size_t>(count);
    client.activity = client.outputProgress = std::chrono::steady_clock::now();
    if (client.sent == bytes.size()) {
      responseBytes -= bytes.size();
      client.output.pop_front();
      client.sent = 0;
    }
    return true;
  }
  bool collectResponses(Client& client) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = client.requests.begin(); it != client.requests.end();) {
      const auto& item = *it;
      if (item->overloaded) return false;
      if (!item->complete && now >= item->deadline) {
        item->cancelled = true;
        complete(item, RpcError(item->request.value("id", Json(nullptr)),
                                "Timed out waiting for an idle editor or changed feedback; "
                                "reread state before retrying"));
        if (item->overloaded) return false;
      }
      if (item->complete) {
        if (client.output.empty()) client.outputProgress = now;
        client.output.push_back(std::move(item->response));
        item->response.clear();
        --outstandingRequests;
        it = client.requests.erase(it);
      } else {
        ++it;
      }
    }
    std::erase_if(pending, [](const auto& item) { return item->cancelled || item->complete; });
    return (!client.closeAfterOutput || !client.requests.empty() || !client.output.empty()) &&
           (client.input.empty() || client.input.find('\n') != std::string::npos ||
            now - client.inputSince < kIoTimeout) &&
           (client.output.empty() || now - client.outputProgress < kIoTimeout) &&
           (now - client.activity < idleTimeout);
  }
  void acceptClient() {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) return;
    if (clients.size() >= kMaximumClients || !SameUserPeer(fd) || !ConfigureNonblocking({fd})) {
      close(fd);
      return;
    }
#ifdef SO_NOSIGPIPE
    int noSignal = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) != 0) {
      close(fd);
      return;
    }
#endif
    clients.push_back(Client{.fd = fd, .agent = AgentSession{.connectionId = nextConnectionId++}});
  }
  void run() {
    while (true) {
      std::vector<pollfd> descriptors{{listener, POLLIN, 0}, {stopPipe[0], POLLIN, 0}};
      bool queued = false;
      {
        std::lock_guard lock(mutex);
        if (stopping) break;
        for (std::size_t i = 0; i < clients.size();) {
          if (!collectResponses(clients[i])) {
            closeClient(i);
            continue;
          }
          queued = parseBuffered(clients[i]) || queued;
          short events = canRead(clients[i]) ? POLLIN : 0;
          if (!clients[i].output.empty()) events |= POLLOUT;
          descriptors.push_back({clients[i].fd, events, 0});
          ++i;
        }
        queued = std::exchange(presenceChanged, false) || queued;
      }
      if (queued) wake();
      const int ready = poll(descriptors.data(), descriptors.size(), 1000);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) break;
      queued = false;
      {
        std::lock_guard lock(mutex);
        if (stopping) break;
        if (descriptors[1].revents & POLLIN) {
          std::array<char, 64> signals{};
          while (read(stopPipe[0], signals.data(), signals.size()) > 0) {}
        }
        // Iterate in reverse so closing a client preserves earlier poll indexes.
        for (std::size_t i = clients.size(); i > 0; --i) {
          Client& client = clients[i - 1];
          const short events = descriptors[i + 1].revents;
          bool valid = (events & (POLLERR | POLLNVAL)) == 0;
          if (valid && (events & (POLLIN | POLLHUP))) valid = receive(client, &queued);
          if (valid && (events & POLLOUT)) valid = sendOutput(client);
          if (!valid) closeClient(i - 1);
        }
        if (descriptors[0].revents & POLLIN) acceptClient();
        queued = std::exchange(presenceChanged, false) || queued;
      }
      if (queued) wake();
    }
    std::lock_guard lock(mutex);
    while (!clients.empty()) closeClient(clients.size() - 1);
    pending.clear();
  }
#endif
};

LocalEditorControl::LocalEditorControl(std::chrono::milliseconds idleTimeout)
    : impl_(std::make_unique<Impl>()) {
  impl_->idleTimeout = idleTimeout;
}
LocalEditorControl::~LocalEditorControl() {
  stop();
}

bool LocalEditorControl::start(std::string socketPath, std::function<void()> wake,
                               std::string* error) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  if (!impl_->canStart(error)) return false;
  if (!ValidSocketAddress(socketPath)) {
    *error = "Control socket must be an absolute path shorter than the platform socket limit";
    return false;
  }
  if (!PrivateSocketDirectory(socketPath)) {
    *error = "Control socket requires an existing directory owned by you with mode 0700";
    return false;
  }
  struct stat existing{};
  if (lstat(socketPath.c_str(), &existing) == 0 || errno != ENOENT) {
    *error = "Control socket path already exists or cannot be inspected; choose a new path";
    return false;
  }
  const int listener = BindPrivateSocket(socketPath, &existing, error);
  if (listener < 0) return false;
  if (!wake || pipe(impl_->stopPipe.data()) != 0) {
    *error = "Cannot create collaboration wake channel";
    close(listener);
    unlink(socketPath.c_str());
    return false;
  }
  if (!ConfigureNonblocking({listener, impl_->stopPipe[0], impl_->stopPipe[1]})) {
    *error = "Cannot configure nonblocking collaboration descriptors";
    close(listener);
    impl_->closeDescriptors();
    unlink(socketPath.c_str());
    return false;
  }
  impl_->path = std::move(socketPath);
  impl_->wake = std::move(wake);
  impl_->stopping = false;
  impl_->device = existing.st_dev;
  impl_->inode = existing.st_ino;
  impl_->listener = listener;
  impl_->worker = std::thread([this]() { impl_->run(); });
  return true;
#else
  (void)socketPath;
  (void)wake;
  *error = "Native collaboration sockets are supported on macOS and Linux";
  return false;
#endif
}

bool LocalEditorControl::process(const Handler& handler) {
  std::deque<std::shared_ptr<Impl::Pending>> batch;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return false;
    batch.swap(impl_->pending);
  }
  bool completed = false;
  for (const auto& item : batch) {
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) break;
      if (item->cancelled || item->complete) continue;
    }
    std::optional<Json> response = handler(item->request, item->agent);
    std::lock_guard lock(impl_->mutex);
    if (item->cancelled || impl_->stopping) continue;
    if (!response) {
      impl_->pending.push_back(item);
    } else {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
      impl_->complete(item, *response);
      impl_->wakeIo();
#endif
      completed = true;
    }
  }
  return completed;
}

bool LocalEditorControl::hasPending() const {
  std::lock_guard lock(impl_->mutex);
  return !impl_->stopping && std::any_of(impl_->pending.begin(), impl_->pending.end(),
                                         [](const auto& item) { return !item->cancelled; });
}

std::vector<LocalEditorControl::AgentSession> LocalEditorControl::connectedAgents() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<AgentSession> agents;
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  if (!impl_->stopping) {
    for (const auto& client : impl_->clients)
      if (client.phase == Impl::Phase::Connected) agents.push_back(client.agent);
  }
#endif
  return agents;
}

void LocalEditorControl::stop() {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  impl_->interrupt();
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->closeDescriptors();
  impl_->removeOwnedSocket();
#endif
  impl_->path.clear();
}
std::string LocalEditorControl::socketPath() const {
  return impl_->path;
}
std::optional<Json> LocalEditorControl::readFeedback() const {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  if (impl_->path.empty()) return std::nullopt;
  std::size_t size = 0;
  const int fd = OpenPrivateFeedback(impl_->path + ".comments.json", &size);
  if (fd < 0) return std::nullopt;
  const auto bytes = ReadFeedbackBytes(fd, size);
  close(fd);
  if (!bytes || !BoundedJsonDepth(*bytes)) return std::nullopt;
  Json value = Json::parse(*bytes, nullptr, false);
  if (value.is_discarded() || !value.is_object()) return std::nullopt;
  return value;
#else
  return std::nullopt;
#endif
}

bool LocalEditorControl::writeFeedback(const Json& data, std::string* error) const {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  const std::string bytes = data.dump(2);
  if (impl_->path.empty() || bytes.size() > 4 * 1024 * 1024) {
    *error = "Feedback storage is unavailable or exceeds 4 MiB";
    return false;
  }
  std::string pattern = impl_->path + ".comments.XXXXXX";
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  const int fd = mkstemp(temporary.data());
  if (fd < 0) {
    *error = "Cannot create private comment checkpoint";
    return false;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  const bool complete = WriteFeedbackBytes(fd, bytes);
  close(fd);
  if (!complete || rename(temporary.data(), (impl_->path + ".comments.json").c_str()) != 0) {
    unlink(temporary.data());
    *error = "Cannot save comments; feedback is still available in this editor session";
    return false;
  }
  return true;
#else
  (void)data;
  *error = "Private comment storage is unavailable on this platform";
  return false;
#endif
}
int RunEditorControlStdio(const std::string& socketPath, int inputFd, int outputFd,
                          std::ostream& errors, EditorControlStdioOptions options) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  return NativeStdioBridge(socketPath, inputFd, outputFd, errors, options).run();
#else
  (void)socketPath;
  (void)inputFd;
  (void)outputFd;
  (void)options;
  errors << "Native editor MCP is supported on macOS and Linux\n";
  return 2;
#endif
}

}  // namespace donner::editor
