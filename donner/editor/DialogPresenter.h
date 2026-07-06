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
  explicit DialogPresenter(std::string editorNoticeText);

  void requestOpenFile(const std::optional<std::string>& currentFilePath);
  void requestSaveFile(const std::optional<std::string>& currentFilePath,
                       std::string error = std::string());
  void requestAbout();

  void render(const std::function<bool(std::string_view, std::string*)>& tryOpenFile,
              const std::function<bool(std::string_view, std::string*)>& trySaveFile);
  void setOpenFileError(std::string error);
  void clearOpenFileError();
  void setSaveFileError(std::string error);
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
};

}  // namespace donner::editor
