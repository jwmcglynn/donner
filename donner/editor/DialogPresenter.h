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
