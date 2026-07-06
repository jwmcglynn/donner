#pragma once
/// @file
///
/// Platform-native file open/save dialogs. On macOS these present
/// `NSOpenPanel` / `NSSavePanel` with a UTType filter for `.svg`, seeded
/// from a remembered default directory, and register chosen files with
/// the OS recent-documents list. On other platforms the functions report
/// unavailable so callers fall back to the in-editor ImGui modal.

#include <optional>
#include <string>
#include <vector>

#include "donner/editor/FileDialogState.h"

struct GLFWwindow;

namespace donner::editor {

/// Parameters for a native open dialog.
struct NativeOpenDialogRequest {
  /// Window title / prompt.
  std::string title = "Open";
  /// Directory to start in, if any.
  std::optional<std::string> defaultDirectory;
  /// Allowed file-type filters. Empty means "any file".
  std::vector<FileDialogFilter> filters;
};

/// Parameters for a native save dialog.
struct NativeSaveDialogRequest {
  /// Window title / prompt.
  std::string title = "Save";
  /// Directory to start in, if any.
  std::optional<std::string> defaultDirectory;
  /// Pre-filled file name (e.g. "untitled.svg"), if any.
  std::optional<std::string> defaultName;
  /// Allowed file-type filters. Empty means "any file".
  std::vector<FileDialogFilter> filters;
};

/// Whether native file dialogs are available on this platform/build.
///
/// True only on macOS; other platforms return false and callers should
/// keep using the ImGui path-entry modal.
[[nodiscard]] bool NativeFileDialogAvailable();

/// Present a native modal open dialog.
///
/// @param parent Owning window (may be nullptr; used for association).
/// @param request Dialog configuration.
/// @return Absolute path of the chosen file, or `std::nullopt` if the
///   user cancelled or native dialogs are unavailable.
[[nodiscard]] std::optional<std::string> ShowNativeOpenFileDialog(
    GLFWwindow* parent, const NativeOpenDialogRequest& request);

/// Present a native modal save dialog.
///
/// @param parent Owning window (may be nullptr; used for association).
/// @param request Dialog configuration.
/// @return Absolute path chosen for saving, or `std::nullopt` if the user
///   cancelled or native dialogs are unavailable.
[[nodiscard]] std::optional<std::string> ShowNativeSaveFileDialog(
    GLFWwindow* parent, const NativeSaveDialogRequest& request);

/// Register \p path with the OS recent-documents list (macOS
/// `NSDocumentController noteNewRecentDocumentURL:`). No-op where native
/// dialogs are unavailable or for empty paths.
void NoteNativeRecentDocument(const std::string& path);

}  // namespace donner::editor
