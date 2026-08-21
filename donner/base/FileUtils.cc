#include "donner/base/FileUtils.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace donner {

namespace {

#ifdef _WIN32

class FileHandle {
public:
  explicit FileHandle(HANDLE handle) : handle_(handle) {}
  ~FileHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }
  HANDLE get() const { return handle_; }

private:
  HANDLE handle_;
};

FileReadResult ReadOpenedFile(const std::filesystem::path& path, size_t maximumSize) {
  FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    return FileReadError::OpenFailed;
  }

  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return FileReadError::OpenFailed;
  }

  LARGE_INTEGER fileSize{};
  if (!GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart < 0 ||
      static_cast<std::uint64_t>(fileSize.QuadPart) > std::numeric_limits<size_t>::max()) {
    return FileReadError::ReadFailed;
  }
  const size_t size = static_cast<size_t>(fileSize.QuadPart);
  if (size > maximumSize) {
    return FileReadError::TooLarge;
  }

  std::string contents(size, '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min(contents.size() - offset, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    DWORD bytesRead = 0;
    if (!ReadFile(file.get(), contents.data() + offset, chunk, &bytesRead, nullptr) ||
        bytesRead == 0) {
      return FileReadError::ReadFailed;
    }
    offset += bytesRead;
  }

  char extra = 0;
  DWORD extraRead = 0;
  if (!ReadFile(file.get(), &extra, 1, &extraRead, nullptr)) {
    return FileReadError::ReadFailed;
  }
  return extraRead == 0 ? FileReadResult(std::move(contents))
                        : FileReadResult(FileReadError::TooLarge);
}

#else

class FileDescriptor {
public:
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  int get() const { return fd_; }

private:
  int fd_;
};

FileReadResult ReadOpenedFile(const std::filesystem::path& path, size_t maximumSize) {
  FileDescriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
  if (file.get() < 0) {
    return FileReadError::OpenFailed;
  }

  struct stat status{};
  if (fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > std::numeric_limits<size_t>::max()) {
    return FileReadError::OpenFailed;
  }
  const size_t size = static_cast<size_t>(status.st_size);
  if (size > maximumSize) {
    return FileReadError::TooLarge;
  }

  std::string contents(size, '\0');
  size_t offset = 0;
  while (offset < contents.size()) {
    const size_t chunk = std::min(contents.size() - offset,
                                  static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t bytesRead = read(file.get(), contents.data() + offset, chunk);
    if (bytesRead < 0 && errno == EINTR) {
      continue;
    }
    if (bytesRead <= 0) {
      return FileReadError::ReadFailed;
    }
    offset += static_cast<size_t>(bytesRead);
  }

  char extra = 0;
  ssize_t extraRead;
  do {
    extraRead = read(file.get(), &extra, 1);
  } while (extraRead < 0 && errno == EINTR);
  if (extraRead < 0) {
    return FileReadError::ReadFailed;
  }
  return extraRead == 0 ? FileReadResult(std::move(contents))
                        : FileReadResult(FileReadError::TooLarge);
}

#endif

}  // namespace

FileReadResult ReadFileBounded(const std::filesystem::path& path, size_t maximumSize) {
  return ReadOpenedFile(path, maximumSize);
}

const char* FileReadErrorMessage(FileReadError error) {
  switch (error) {
    case FileReadError::OpenFailed: return "could not open regular file";
    case FileReadError::TooLarge: return "file exceeds maximum input size";
    case FileReadError::ReadFailed: return "could not read file completely";
  }
  return "unknown file read error";
}

}  // namespace donner
