#pragma once
/// @file

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace donner::editor {

/// Result category for saving an SVG source file.
enum class DocumentSaveStatus {
  Ok,           ///< The source was written successfully.
  OpenFailed,   ///< The destination could not be opened.
  WriteFailed,  ///< The full source could not be written.
  CloseFailed,  ///< Closing the file descriptor failed after writing.
};

/// Human-readable stream output for \ref DocumentSaveStatus.
std::ostream& operator<<(std::ostream& os, DocumentSaveStatus status);

/// Detailed outcome from a document save attempt.
struct DocumentSaveResult {
  DocumentSaveStatus status = DocumentSaveStatus::Ok;
  int errorNumber = 0;
  std::size_t bytesWritten = 0;
  std::string message;

  /// Whether the save completed successfully.
  [[nodiscard]] bool ok() const { return status == DocumentSaveStatus::Ok; }
};

/**
 * Write SVG source bytes to \p path without following symlinks.
 *
 * Existing destinations are opened with `O_NOFOLLOW`; missing destinations are created with
 * `O_CREAT | O_EXCL`. The function intentionally does not pre-stat the path, avoiding a TOCTOU
 * window between inspection and open.
 *
 * @param path Destination file path.
 * @param source SVG source bytes to persist.
 * @return Save status, including `errno` and a diagnostic message on failure.
 */
[[nodiscard]] DocumentSaveResult SaveSourceToPath(const std::filesystem::path& path,
                                                  std::string_view source);

}  // namespace donner::editor
