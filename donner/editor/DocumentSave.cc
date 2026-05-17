#include "donner/editor/DocumentSave.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ostream>
#include <string>

namespace donner::editor {

namespace {

constexpr int kFileMode = 0666;

int OpenForSaveNoSymlink(const std::filesystem::path& path) {
  const std::string nativePath = path.string();
  int flags = O_WRONLY | O_TRUNC;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif

  int fd = open(nativePath.c_str(), flags);
  if (fd >= 0 || errno != ENOENT) {
    return fd;
  }

  flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  return open(nativePath.c_str(), flags, kFileMode);
}

std::string BuildErrorMessage(std::string_view prefix, const std::filesystem::path& path,
                              int errorNumber) {
  std::string message(prefix);
  message += " ";
  message += path.string();
  message += ": ";
  message += std::strerror(errorNumber);
  return message;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, DocumentSaveStatus status) {
  switch (status) {
    case DocumentSaveStatus::Ok: return os << "Ok";
    case DocumentSaveStatus::OpenFailed: return os << "OpenFailed";
    case DocumentSaveStatus::WriteFailed: return os << "WriteFailed";
    case DocumentSaveStatus::CloseFailed: return os << "CloseFailed";
  }

  return os << "Unknown";
}

DocumentSaveResult SaveSourceToPath(const std::filesystem::path& path, std::string_view source) {
  const int fd = OpenForSaveNoSymlink(path);
  if (fd < 0) {
    const int errorNumber = errno;
    return DocumentSaveResult{
        .status = DocumentSaveStatus::OpenFailed,
        .errorNumber = errorNumber,
        .message = BuildErrorMessage("Could not open", path, errorNumber),
    };
  }

  std::size_t bytesWritten = 0;
  while (bytesWritten < source.size()) {
    const std::string_view remaining = source.substr(bytesWritten);
    const ssize_t written = write(fd, remaining.data(), remaining.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }

      const int errorNumber = errno;
      close(fd);
      return DocumentSaveResult{
          .status = DocumentSaveStatus::WriteFailed,
          .errorNumber = errorNumber,
          .bytesWritten = bytesWritten,
          .message = BuildErrorMessage("Could not write", path, errorNumber),
      };
    }
    if (written == 0) {
      close(fd);
      return DocumentSaveResult{
          .status = DocumentSaveStatus::WriteFailed,
          .errorNumber = EIO,
          .bytesWritten = bytesWritten,
          .message = BuildErrorMessage("Could not write", path, EIO),
      };
    }

    bytesWritten += static_cast<std::size_t>(written);
  }

  if (close(fd) != 0) {
    const int errorNumber = errno;
    return DocumentSaveResult{
        .status = DocumentSaveStatus::CloseFailed,
        .errorNumber = errorNumber,
        .bytesWritten = bytesWritten,
        .message = BuildErrorMessage("Could not close", path, errorNumber),
    };
  }

  return DocumentSaveResult{
      .status = DocumentSaveStatus::Ok,
      .bytesWritten = bytesWritten,
  };
}

}  // namespace donner::editor
