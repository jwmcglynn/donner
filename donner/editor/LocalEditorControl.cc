#include "donner/editor/LocalEditorControl.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
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
Json RpcError(const Json& id, std::string message) {
  return {{"jsonrpc", "2.0"},
          {"id", id},
          {"error", {{"code", -32000}, {"message", std::move(message)}}}};
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

  bool sameUser(int fd) {
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
    if (!sameUser(fd)) return;
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
                               "Editor unavailable; reread document state before retrying"));
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
}  // namespace donner::editor
