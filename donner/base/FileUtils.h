#pragma once
/// @file

#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>

namespace donner {

/// Failure returned by \ref donner::ReadFileBounded "ReadFileBounded".
enum class FileReadError {
  OpenFailed,  ///< The path could not be opened, or does not name a regular file.
  TooLarge,    ///< The file is larger than the maximum size, or grew while being read.
  ReadFailed,  ///< A read failed, or the file ended before its reported size.
};

/// Contents or failure from a bounded file read.
using FileReadResult = std::variant<std::string, FileReadError>;

/**
 * Open and read a regular file without allocating more than \p maximumSize bytes.
 *
 * The read uses a nonblocking descriptor or handle, follows the requested final symlink, and
 * validates the opened object as a regular file. It fails closed for directories, FIFOs, devices,
 * size changes, and data beyond the expected byte count.
 *
 * @param path File to read.
 * @param maximumSize Maximum accepted byte count.
 */
FileReadResult ReadFileBounded(const std::filesystem::path& path, size_t maximumSize);

/// Human-readable description for \p error.
const char* FileReadErrorMessage(FileReadError error);

}  // namespace donner
