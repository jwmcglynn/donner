#pragma once
/// @file
///
/// Pure, platform-independent bookkeeping for the editor's native file
/// dialogs: default-directory memory and an in-process recent-files list.
///
/// The actual `NSOpenPanel` / `NSSavePanel` presentation lives in the
/// platform layer (`NativeFileDialog.h`); this module holds only the
/// logic that can be exercised headlessly under unit test.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor {

/// A single file-type filter offered by a native open/save dialog.
struct FileDialogFilter {
  /// Human-readable description shown in the dialog (e.g. "SVG Image").
  std::string description;
  /// Lowercase file extensions without the leading dot (e.g. {"svg"}).
  std::vector<std::string> extensions;
};

/// The editor's canonical filter set: SVG documents.
[[nodiscard]] std::vector<FileDialogFilter> SvgFileDialogFilters();

/// Tracks the directory the user last browsed to and the most-recently
/// opened/saved files, so the next dialog can seed a sensible starting
/// directory and (on platforms without an OS recents list) an in-editor
/// recent-files menu.
class FileDialogState {
public:
  /// @param maxRecents Maximum number of remembered recent files.
  explicit FileDialogState(std::size_t maxRecents = 10);

  /// Directory to seed the next dialog with.
  ///
  /// Preference order: the directory of the last chosen file, then the
  /// parent directory of \p currentFilePath if one is open, then empty
  /// (let the OS pick its own default).
  ///
  /// @param currentFilePath Path of the document currently open, if any.
  [[nodiscard]] std::optional<std::string> defaultDirectory(
      const std::optional<std::string>& currentFilePath) const;

  /// Record that the user opened or saved \p path. Updates the remembered
  /// directory and pushes the file to the front of the recents list
  /// (de-duplicated, most-recent first, bounded by `maxRecents`).
  ///
  /// Empty paths are ignored.
  void noteChosenPath(std::string_view path);

  /// Most-recently opened/saved files, newest first.
  [[nodiscard]] const std::vector<std::string>& recentFiles() const { return recentFiles_; }

  /// Directory remembered from the last chosen file, if any.
  [[nodiscard]] const std::optional<std::string>& lastDirectory() const { return lastDirectory_; }

private:
  std::size_t maxRecents_;
  std::optional<std::string> lastDirectory_;
  std::vector<std::string> recentFiles_;
};

}  // namespace donner::editor
