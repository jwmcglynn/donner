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
  int listener = -1;
  std::array<int, 2> stopPipe{-1, -1};
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  static constexpr std::size_t kMaximumClients = 8;
  static constexpr std::size_t kMaximumClientMessages = 8;
  static constexpr std::size_t kMaximumPendingRequests = 32;
  static constexpr std::size_t kMaximumQueuedResponseBytes = 32 * 1024 * 1024;
  static constexpr auto kIoTimeout = std::chrono::seconds(10);
  static constexpr auto kIdleTimeout = std::chrono::seconds(60);
  struct Client {
    int fd;
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
      pending.push_back(item);
      queued = true;
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
           (now - client.activity < kIdleTimeout);
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
    clients.push_back(Client{.fd = fd});
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
      }
      if (queued) wake();
    }
    std::lock_guard lock(mutex);
    while (!clients.empty()) closeClient(clients.size() - 1);
    pending.clear();
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

bool LocalEditorControl::process(const std::function<std::optional<Json>(const Json&)>& handler) {
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
    std::optional<Json> response = handler(item->request);
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
