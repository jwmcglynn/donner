#include "donner/editor/LocalEditorControl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
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
  if (chmod(path.c_str(), 0600) != 0 || listen(fd, 4) != 0 || lstat(path.c_str(), identity) != 0) {
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

bool SendClientRequest(int fd, const std::string& bytes, SocketDeadline deadline) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (!WaitSocket(fd, POLLOUT, deadline)) return false;
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const ssize_t count = send(fd, bytes.data() + offset, bytes.size() - offset, flags);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<Json> ReadClientResponse(int fd, SocketDeadline deadline, std::string* error) {
  std::string bytes;
  std::array<char, 8192> buffer{};
  while (WaitSocket(fd, POLLIN, deadline)) {
    const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) break;
    if (bytes.size() + static_cast<std::size_t>(count) > kMaximumResponseBytes + 1) {
      *error = "Editor response exceeds 16 MiB";
      return std::nullopt;
    }
    bytes.append(buffer.data(), static_cast<std::size_t>(count));
    const auto newline = bytes.find('\n');
    if (newline == std::string::npos) continue;
    if (newline + 1 != bytes.size() || !BoundedJsonDepth(bytes)) {
      *error = "Invalid editor response framing or nesting";
      return std::nullopt;
    }
    Json response = Json::parse(bytes, nullptr, false);
    if (response.is_discarded() || (!response.is_object() && !response.is_null())) {
      *error = "Editor response must be a JSON object";
      return std::nullopt;
    }
    return response;
  }
  *error = "Editor connection ended or timed out; reread state before retrying edits";
  return std::nullopt;
}

std::optional<Json> RequestEditor(const std::string& path, const Json& request,
                                  std::string* error) {
  struct stat endpoint{};
  if (!ValidSocketAddress(path) || !PrivateSocketDirectory(path) ||
      lstat(path.c_str(), &endpoint) != 0 || !S_ISSOCK(endpoint.st_mode) ||
      endpoint.st_uid != geteuid() || (endpoint.st_mode & 0077) != 0) {
    *error = "Editor endpoint and directory must be private and owned by the current user";
    return std::nullopt;
  }
  ClientSocket connection{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (connection.fd < 0 || fcntl(connection.fd, F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(connection.fd, F_SETFL, O_NONBLOCK) != 0) {
    *error = "Cannot create editor connection";
    return std::nullopt;
  }
#ifdef SO_NOSIGPIPE
  int noSignal = 1;
  setsockopt(connection.fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
  const int connected =
      connect(connection.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  int socketError = 0;
  socklen_t length = sizeof(socketError);
  if ((connected != 0 && errno != EINPROGRESS && errno != EINTR) ||
      !WaitSocket(connection.fd, POLLOUT, deadline) ||
      getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 ||
      socketError != 0 || !SameUserPeer(connection.fd)) {
    *error = "Cannot connect to the private editor endpoint";
    return std::nullopt;
  }
  const std::string payload = request.dump() + "\n";
  if (payload.size() > kMaximumRequestBytes + 1 ||
      !SendClientRequest(connection.fd, payload, deadline)) {
    *error = "Cannot send the bounded editor request";
    return std::nullopt;
  }
  auto response = ReadClientResponse(connection.fd, deadline, error);
  if (request.contains("id") && response &&
      (!response->is_object() || !response->contains("id") || (*response)["id"] != request["id"])) {
    *error = "Editor response ID does not match the request";
    return std::nullopt;
  }
  return response;
}

#endif
}  // namespace

struct LocalEditorControl::Impl {
  struct Pending {
    Json request;
    Json response;
    bool complete = false;
    bool cancelled = false;
  };
  std::string path;
  std::function<void()> wake;
  std::thread worker;
  std::mutex mutex;
  std::condition_variable condition;
  std::shared_ptr<Pending> pending;
  bool stopping = false;
  int listener = -1;
  int client = -1;
  std::array<int, 2> stopPipe{-1, -1};
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  dev_t device = 0;
  ino_t inode = 0;

  void interrupt() {
    std::lock_guard lock(mutex);
    stopping = true;
    if (client >= 0) shutdown(client, SHUT_RDWR);
    if (stopPipe[1] >= 0) {
      const char signal = 1;
      (void)write(stopPipe[1], &signal, 1);
    }
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

  static bool sendReply(int fd, const Json& response) {
    std::string bytes = response.dump();
    if (bytes.size() > kMaximumResponseBytes)
      bytes = RpcError(nullptr, "Response exceeds 16 MiB").dump();
    bytes += '\n';
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const ssize_t count = send(fd, bytes.data() + sent, bytes.size() - sent, flags);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      sent += static_cast<std::size_t>(count);
    }
    return true;
  }

  void serveClient(int fd) {
    if (!SameUserPeer(fd)) return;
    timeval timeout{.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int noSignal = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
    std::string bytes;
    std::array<char, 4096> buffer{};
    while (bytes.size() <= kMaximumRequestBytes) {
      const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return;
      bytes.append(buffer.data(), static_cast<std::size_t>(count));
      const auto newline = bytes.find('\n');
      if (newline == std::string::npos) continue;
      if (newline > kMaximumRequestBytes || newline + 1 != bytes.size()) {
        sendReply(fd, RpcError(nullptr, "Expected one bounded JSON request"));
        return;
      }
      if (!BoundedJsonDepth(bytes)) {
        sendReply(fd, RpcError(nullptr, "JSON nesting exceeds 64 levels"));
        return;
      }
      Json request = Json::parse(
          bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(newline), nullptr, false);
      if (request.is_discarded() || !request.is_object()) {
        sendReply(fd, RpcError(nullptr, "Invalid JSON object"));
        return;
      }
      auto item = std::make_shared<Pending>();
      item->request = std::move(request);
      {
        std::lock_guard lock(mutex);
        if (stopping) return;
        pending = item;
      }
      wake();
      std::unique_lock lock(mutex);
      condition.wait_for(lock, kRequestTimeout, [&]() { return stopping || item->complete; });
      if (!item->complete) {
        item->cancelled = true;
        if (pending == item) pending.reset();
        lock.unlock();
        sendReply(fd, RpcError(item->request.value("id", Json(nullptr)),
                               "Timed out waiting for an idle editor or changed feedback; reread "
                               "state before retrying"));
        return;
      }
      Json response = std::move(item->response);
      lock.unlock();
      sendReply(fd, response);
      return;
    }
    sendReply(fd, RpcError(nullptr, "Request exceeds 1 MiB"));
  }

  void run() {
    while (true) {
      std::array<pollfd, 2> descriptors{{{listener, POLLIN, 0}, {stopPipe[0], POLLIN, 0}}};
      const int ready = poll(descriptors.data(), descriptors.size(), -1);
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || descriptors[1].revents != 0) return;
      const int fd = accept(listener, nullptr, nullptr);
      if (fd < 0) {
        if (errno == EINTR) continue;
        return;
      }
      {
        std::lock_guard lock(mutex);
        if (stopping) {
          close(fd);
          return;
        }
        client = fd;
      }
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      serveClient(fd);
      {
        std::lock_guard lock(mutex);
        client = -1;
        close(fd);
        if (stopping) return;
      }
    }
  }
#endif
};

LocalEditorControl::LocalEditorControl() : impl_(std::make_unique<Impl>()) {}
LocalEditorControl::~LocalEditorControl() {
  stop();
}

bool LocalEditorControl::start(std::string socketPath, std::function<void()> wake,
                               std::string* error) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  if (impl_->worker.joinable()) {
    *error = "Collaboration endpoint is already running";
    return false;
  }
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
  fcntl(listener, F_SETFD, FD_CLOEXEC);
  fcntl(impl_->stopPipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(impl_->stopPipe[1], F_SETFD, FD_CLOEXEC);
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

bool LocalEditorControl::process(const std::function<std::optional<Json>(const Json&)>& handler) {
  std::shared_ptr<Impl::Pending> item;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || !impl_->pending || impl_->pending->cancelled) return false;
    item = std::move(impl_->pending);
  }
  std::optional<Json> response = handler(item->request);
  if (!response) {
    std::lock_guard lock(impl_->mutex);
    if (!item->cancelled && !impl_->stopping) impl_->pending = item;
    return false;
  }
  {
    std::lock_guard lock(impl_->mutex);
    item->response = std::move(*response);
    item->complete = true;
  }
  impl_->condition.notify_all();
  return true;
}

bool LocalEditorControl::hasPending() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->pending && !impl_->pending->cancelled && !impl_->stopping;
}

void LocalEditorControl::stop() {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  impl_->interrupt();
  impl_->condition.notify_all();
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
int RunEditorControlStdio(const std::string& socketPath, std::istream& input, std::ostream& output,
                          std::ostream& errors) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  if (!ValidSocketAddress(socketPath)) {
    errors << "MCP requires an absolute private socket path within the platform limit\n";
    return 2;
  }
  while (true) {
    std::string line;
    char byte = 0;
    bool terminated = false;
    while (line.size() <= kMaximumRequestBytes && input.get(byte)) {
      if (byte == '\n') {
        terminated = true;
        break;
      }
      line.push_back(byte);
    }
    if (line.empty() && input.eof()) return 0;
    if (!terminated || line.size() > kMaximumRequestBytes || !BoundedJsonDepth(line)) {
      output << RpcError(nullptr, "MCP input must be bounded newline-delimited JSON", -32700).dump()
             << '\n'
             << std::flush;
      return 2;
    }
    Json request = Json::parse(line, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
      output << RpcError(nullptr, "MCP input must be a JSON object", -32700).dump() << '\n'
             << std::flush;
      return 2;
    }
    const auto version = request.find("jsonrpc");
    const bool validId =
        !request.contains("id") || request["id"].is_string() || request["id"].is_number_integer();
    if (version == request.end() || !version->is_string() || *version != "2.0" || !validId) {
      output << RpcError(nullptr, "Invalid MCP JSON-RPC version or request ID", -32600).dump()
             << '\n'
             << std::flush;
      return 2;
    }
    std::string error;
    std::optional<Json> response = RequestEditor(socketPath, request, &error);
    if (!error.empty()) {
      errors << error << '\n';
      if (request.contains("id")) response = RpcError(request["id"], std::move(error));
    }
    if (request.contains("id") && response) {
      output << response->dump(-1, ' ', false, Json::error_handler_t::replace) << '\n'
             << std::flush;
      if (!output) return 1;
    }
  }
#else
  (void)socketPath;
  (void)input;
  (void)output;
  errors << "Native editor MCP is supported on macOS and Linux\n";
  return 2;
#endif
}

}  // namespace donner::editor
