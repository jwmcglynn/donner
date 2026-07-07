#pragma once
/// @file
///
/// Bridges the pure `FileDialogState` bookkeeping with the platform
/// `NativeFileDialog` panels. The editor shell holds one of these and asks
/// it to present open/save dialogs; the coordinator seeds the dialog with
/// the remembered directory, records the chosen file for directory memory
/// and recents, and registers it with the OS recent-documents list.

#include <optional>
#include <string>
#include <vector>

#include "donner/editor/FileDialogState.h"

struct GLFWwindow;

namespace donner::editor {

/// Presents native open/save dialogs and tracks directory/recents state.
class NativeDialogCoordinator {
public:
  /// Whether native dialogs are available on this platform. When false,
  /// callers should fall back to the in-editor ImGui modal.
  [[nodiscard]] bool available() const;

  /// Present a native "Open SVG" dialog.
  ///
  /// @param window Owning editor window.
  /// @param currentFilePath Path of the open document, used to seed the
  ///   start directory when nothing better is remembered.
  /// @return Chosen absolute path (already recorded into directory memory
  ///   and recents), or `std::nullopt` if cancelled/unavailable.
  [[nodiscard]] std::optional<std::string> openFile(
      GLFWwindow* window, const std::optional<std::string>& currentFilePath);

  /// Present a native "Save SVG" dialog.
  ///
  /// @param window Owning editor window.
  /// @param suggestedPath Path or name to pre-fill; its directory seeds the
  ///   start directory and its filename seeds the name field. When empty,
  ///   defaults to "untitled.svg".
  /// @return Chosen absolute path (already recorded), or `std::nullopt` if
  ///   cancelled/unavailable.
  [[nodiscard]] std::optional<std::string> saveFile(
      GLFWwindow* window, const std::optional<std::string>& suggestedPath);

  /// In-process recent-files list, newest first.
  [[nodiscard]] const std::vector<std::string>& recentFiles() const {
    return state_.recentFiles();
  }

private:
  void recordChosen(const std::string& path);

  FileDialogState state_;
};

}  // namespace donner::editor
