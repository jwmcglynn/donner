#pragma once
/// @file

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace donner::editor {

/// Owns the editor's popup/modal state and renders the corresponding ImGui dialogs.
class DialogPresenter {
public:
  /// Create modal state with the host's notice and build information.
  /// @param editorNoticeText Third-party notices shown in the Licenses dialog.
  /// @param editorBuildInfo Optional version and commit lines.
  explicit DialogPresenter(std::string editorNoticeText,
                           std::string editorBuildInfo = std::string());

  /// Queue the Open SVG dialog with an initial path.
  /// @param currentFilePath Current file path when available.
  void requestOpenFile(const std::optional<std::string>& currentFilePath);

  /// Queue the Save SVG dialog with an initial path and optional error.
  /// @param currentFilePath Current or suggested destination path.
  /// @param error Initial save error to display.
  void requestSaveFile(const std::optional<std::string>& currentFilePath,
                       std::string error = std::string());

  /// Queue the About dialog for the next presentation pass.
  void requestAbout();

  /// Draw requested dialogs and invoke host callbacks for accepted paths.
  /// @param tryOpenFile Attempts an open and returns success; writes a failure message through its
  /// string pointer.
  /// @param trySaveFile Attempts a save and returns success; writes a failure message through its
  /// string pointer.
  void render(const std::function<bool(std::string_view, std::string*)>& tryOpenFile,
              const std::function<bool(std::string_view, std::string*)>& trySaveFile);

  /// Replace the error shown by the Open SVG dialog.
  /// @param error New diagnostic text.
  void setOpenFileError(std::string error);

  /// Clear the Open SVG dialog error.
  void clearOpenFileError();

  /// Replace the error shown by the Save SVG dialog.
  /// @param error New diagnostic text.
  void setSaveFileError(std::string error);

  /// Clear the Save SVG dialog error.
  void clearSaveFileError();

  /// Whether an Open SVG modal has been requested but not yet opened by render().
  [[nodiscard]] bool openFileModalRequested() const { return openFileModalRequested_; }

  /// Whether a Save SVG modal has been requested but not yet opened by render().
  [[nodiscard]] bool saveFileModalRequested() const { return saveFileModalRequested_; }

  /// Consume a pending Open SVG request without showing the ImGui modal.
  /// Used when a native OS dialog handles the interaction instead.
  void consumeOpenFileModalRequest() { openFileModalRequested_ = false; }

  /// Consume a pending Save SVG request without showing the ImGui modal.
  /// Used when a native OS dialog handles the interaction instead.
  void consumeSaveFileModalRequest() { saveFileModalRequested_ = false; }

  /// The path pre-filled into the pending Save SVG request (the current file
  /// path, or the suggested export/save-as name). Empty when none was set.
  [[nodiscard]] std::string pendingSaveFilePath() const {
    return std::string(saveFilePathBuffer_.data());
  }

  /// Whether the About modal has been requested but not yet opened by render().
  [[nodiscard]] bool aboutPopupRequested() const { return openAboutPopup_; }

private:
  bool openFileModalRequested_ = false;
  bool saveFileModalRequested_ = false;
  bool openAboutPopup_ = false;
  bool openLicensesPopup_ = false;
  std::array<char, 4096> openFilePathBuffer_{};
  std::array<char, 4096> saveFilePathBuffer_{};
  std::string openFileError_;
  std::string saveFileError_;
  std::string editorNoticeText_;
  /// Embedded "<version>\n<commit>\n" build metadata, or empty when unset.
  std::string editorBuildInfo_;
  /// Version line parsed from \ref editorBuildInfo_, or empty when unset.
  std::string editorVersion_;
  /// Commit line parsed from \ref editorBuildInfo_, or empty when unset.
  std::string editorCommit_;
};

}  // namespace donner::editor
