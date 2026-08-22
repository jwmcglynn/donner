#include "donner/svg/resources/SandboxedFileResourceLoader.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "donner/base/Utf8.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

struct SandboxedFileResourceLoader::RootDirectoryHandle {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;

  ~RootDirectoryHandle() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
#else
  int fd = -1;

  ~RootDirectoryHandle() {
    if (fd >= 0) {
      close(fd);
    }
  }
#endif
};

namespace {

bool PathComponentsEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
#ifdef _WIN32
  return _wcsicmp(lhs.c_str(), rhs.c_str()) == 0;
#else
  return lhs == rhs;
#endif
}

bool PathContainsNull(const std::filesystem::path& path) {
  return path.native().find(std::filesystem::path::value_type{}) !=
         std::filesystem::path::string_type::npos;
}

bool IsBoundedUrlPath(std::string_view url) {
  if (url.size() > SandboxedFileResourceLoader::kMaximumPathBytes ||
      url.find('\0') != std::string_view::npos || !Utf8::IsValidString(url)) {
    return false;
  }

  std::size_t components = 0;
  bool inComponent = false;
  for (const char ch : url) {
    if (ch == '/' || ch == '\\') {
      inComponent = false;
    } else if (!inComponent) {
      inComponent = true;
      if (++components > SandboxedFileResourceLoader::kMaximumPathComponents) {
        return false;
      }
    }
  }
  return true;
}

bool IsPathUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
  assert(root.is_absolute());
  assert(path.is_absolute());

  auto rootIt = root.begin();
  auto pathIt = path.begin();
  for (; rootIt != root.end() && pathIt != path.end(); ++rootIt, ++pathIt) {
    if (!PathComponentsEqual(*rootIt, *pathIt)) {
      return false;
    }
  }
  return rootIt == root.end();
}

struct FileCloser {
  void operator()(std::FILE* file) const { std::fclose(file); }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

#ifdef _WIN32

std::optional<std::filesystem::path> FinalPathForHandle(HANDLE handle) {
  const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
  if (required == 0) {
    return std::nullopt;
  }

  std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
  const DWORD written =
      GetFinalPathNameByHandleW(handle, buffer.data(), required + 1, FILE_NAME_NORMALIZED);
  if (written == 0 || written > required) {
    return std::nullopt;
  }
  return std::filesystem::path(std::wstring(buffer.data(), written));
}

std::variant<FilePtr, ResourceLoaderError> OpenSandboxedBinaryFile(
    HANDLE rootHandle, const std::filesystem::path& path) {
  // Omitting FILE_SHARE_DELETE pins the opened file and the root directory at their current names
  // while the handle-based containment decision and read are in progress.
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return ResourceLoaderError::NotFound;
  }

  const auto rootPath = FinalPathForHandle(rootHandle);
  const auto openedPath = FinalPathForHandle(handle);
  if (!rootPath || !openedPath || !IsPathUnderRoot(*rootPath, *openedPath)) {
    CloseHandle(handle);
    return ResourceLoaderError::SandboxViolation;
  }

  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_RDONLY);
  if (fd < 0) {
    CloseHandle(handle);
    return ResourceLoaderError::NotFound;
  }
  std::FILE* file = _fdopen(fd, "rb");
  if (file == nullptr) {
    _close(fd);
    return ResourceLoaderError::NotFound;
  }
  return FilePtr(file);
}

#else

std::variant<FilePtr, ResourceLoaderError> OpenSandboxedBinaryFile(
    int rootFd, const std::filesystem::path& relativePath) {
  std::vector<std::filesystem::path> components;
  for (const auto& component : relativePath) {
    if (component.empty() || component == ".") {
      continue;
    }
    if (component == "..") {
      return ResourceLoaderError::SandboxViolation;
    }
    if (PathContainsNull(component)) {
      return ResourceLoaderError::SandboxViolation;
    }
    components.push_back(component);
  }
  if (components.empty()) {
    return ResourceLoaderError::NotFound;
  }

  int currentFd = dup(rootFd);
  if (currentFd < 0) {
    return ResourceLoaderError::NotFound;
  }
  (void)fcntl(currentFd, F_SETFD, FD_CLOEXEC);

  for (size_t i = 0; i < components.size(); ++i) {
    const bool isLast = i + 1 == components.size();
    struct stat linkStatus{};
    if (fstatat(currentFd, components[i].c_str(), &linkStatus, AT_SYMLINK_NOFOLLOW) != 0) {
      close(currentFd);
      return ResourceLoaderError::NotFound;
    }
    if (S_ISLNK(linkStatus.st_mode)) {
      close(currentFd);
      return ResourceLoaderError::SandboxViolation;
    }

    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    if (!isLast) {
      flags |= O_DIRECTORY;
    } else {
      // Avoid blocking indefinitely if an attacker replaces the final entry with a FIFO or device.
      // O_NONBLOCK is inert for regular files, which are verified after opening.
      flags |= O_NONBLOCK;
    }
    const int nextFd = openat(currentFd, components[i].c_str(), flags);
    const int openError = errno;
    close(currentFd);
    if (nextFd < 0) {
      return openError == ELOOP ? ResourceLoaderError::SandboxViolation
                                : ResourceLoaderError::NotFound;
    }
    currentFd = nextFd;
  }

  std::FILE* file = fdopen(currentFd, "rb");
  if (file == nullptr) {
    close(currentFd);
    return ResourceLoaderError::NotFound;
  }
  return FilePtr(file);
}

#endif

struct OpenedFileInfo {
  uintmax_t size = 0;
  bool regular = false;
};

std::optional<OpenedFileInfo> GetOpenedFileInfo(std::FILE* file) {
#ifdef _WIN32
  struct _stat64 status{};
  if (_fstat64(_fileno(file), &status) != 0 || status.st_size < 0) {
    return std::nullopt;
  }
  return OpenedFileInfo{
      .size = static_cast<uintmax_t>(status.st_size),
      .regular = (status.st_mode & _S_IFMT) == _S_IFREG,
  };
#else
  struct stat status{};
  if (fstat(fileno(file), &status) != 0 || status.st_size < 0) {
    return std::nullopt;
  }
  return OpenedFileInfo{
      .size = static_cast<uintmax_t>(status.st_size),
      .regular = S_ISREG(status.st_mode),
  };
#endif
}

std::optional<std::filesystem::path> ResolveCanonicalRoot(const std::filesystem::path& root) {
  if (PathContainsNull(root)) {
    return std::nullopt;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return std::nullopt;
  }
  std::filesystem::path canonicalRoot = std::filesystem::canonical(root, error);
  if (error || !canonicalRoot.is_absolute()) {
    return std::nullopt;
  }
  return canonicalRoot;
}

std::optional<std::filesystem::path> ResolveDocumentDirectory(
    const std::filesystem::path& root, const std::filesystem::path& documentPath) {
  if (PathContainsNull(documentPath)) {
    return std::nullopt;
  }
  std::error_code error;
  const std::filesystem::path absoluteDocumentPath =
      std::filesystem::absolute(documentPath, error).lexically_normal();
  if (error || !absoluteDocumentPath.is_absolute()) {
    return std::nullopt;
  }

  std::filesystem::path directory;
  const std::filesystem::path canonicalDocumentPath =
      std::filesystem::canonical(absoluteDocumentPath, error);
  if (!error) {
    directory = canonicalDocumentPath.parent_path();
  } else {
    error.clear();
    directory = std::filesystem::canonical(absoluteDocumentPath.parent_path(), error);
  }
  if (error || !directory.is_absolute() || !IsPathUnderRoot(root, directory)) {
    return std::nullopt;
  }
  return directory;
}

std::variant<std::filesystem::path, ResourceLoaderError> ResolveSandboxedPath(
    const std::filesystem::path& root, const std::filesystem::path& documentPath,
    std::string_view url) {
  if (!IsBoundedUrlPath(url)) {
    return ResourceLoaderError::SandboxViolation;
  }
  std::filesystem::path path(url);
  if (!path.is_absolute()) {
    path = documentPath / path;
  }
  std::error_code error;
  path = std::filesystem::absolute(path, error).lexically_normal();
  if (error) {
    return ResourceLoaderError::NotFound;
  }
  if (!IsPathUnderRoot(root, path)) {
    return ResourceLoaderError::SandboxViolation;
  }
  return path;
}

std::variant<std::vector<uint8_t>, ResourceLoaderError> ReadBoundedFile(FilePtr file,
                                                                        size_t maximumSize) {
  const auto opened = GetOpenedFileInfo(file.get());
  if (!opened || !opened->regular || opened->size > std::numeric_limits<size_t>::max()) {
    return ResourceLoaderError::NotFound;
  }
  if (opened->size > maximumSize) {
    return ResourceLoaderError::TooLarge;
  }

  std::vector<uint8_t> data(static_cast<size_t>(opened->size));
  const size_t bytesRead = std::fread(data.data(), 1, data.size(), file.get());
  if (bytesRead != data.size() || std::ferror(file.get())) {
    return ResourceLoaderError::NotFound;
  }
  if (std::fgetc(file.get()) != EOF) {
    return ResourceLoaderError::TooLarge;
  }
  return data;
}

}  // namespace

SandboxedFileResourceLoader::SandboxedFileResourceLoader(const std::filesystem::path& root,
                                                         const std::filesystem::path& documentPath,
                                                         size_t maximumResourceSize)
    : maximumResourceSize_(maximumResourceSize),
      rootHandle_(std::make_unique<RootDirectoryHandle>()) {
  const auto canonicalRoot = ResolveCanonicalRoot(root);
  if (!canonicalRoot) {
    return;
  }
  root_ = *canonicalRoot;
  const auto documentDirectory = ResolveDocumentDirectory(root_, documentPath);
  if (!documentDirectory) {
    root_.clear();
    return;
  }
  documentPath_ = *documentDirectory;
#ifdef _WIN32
  rootHandle_->handle = CreateFileW(
      root_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (rootHandle_->handle == INVALID_HANDLE_VALUE) {
    root_.clear();
  }
#else
  rootHandle_->fd = open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (rootHandle_->fd < 0) {
    root_.clear();
  }
#endif
}

SandboxedFileResourceLoader::~SandboxedFileResourceLoader() = default;

bool SandboxedFileResourceLoader::hasValidRootHandle() const {
#ifdef _WIN32
  return rootHandle_->handle != INVALID_HANDLE_VALUE;
#else
  return rootHandle_->fd >= 0;
#endif
}

std::variant<std::vector<uint8_t>, ResourceLoaderError>
SandboxedFileResourceLoader::fetchExternalResource(std::string_view url) {
  if (root_.empty()) {
    return ResourceLoaderError::NotFound;
  }
  if (!hasValidRootHandle()) {
    return ResourceLoaderError::NotFound;
  }
  auto maybePath = ResolveSandboxedPath(root_, documentPath_, url);
  if (const auto* error = std::get_if<ResourceLoaderError>(&maybePath)) {
    return *error;
  }
  const std::filesystem::path& path = std::get<std::filesystem::path>(maybePath);

#ifdef _WIN32
  auto openResult = OpenSandboxedBinaryFile(rootHandle_->handle, path);
#else
  auto openResult = OpenSandboxedBinaryFile(rootHandle_->fd, path.lexically_relative(root_));
#endif
  if (const auto* error = std::get_if<ResourceLoaderError>(&openResult)) {
    return *error;
  }
  return ReadBoundedFile(std::move(std::get<FilePtr>(openResult)), maximumResourceSize_);
}

}  // namespace donner::svg
