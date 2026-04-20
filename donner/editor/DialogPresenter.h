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
  void requestAbout();

  void render(const std::function<bool(std::string_view, std::string*)>& tryOpenFile);
  void setOpenFileError(std::string error);
  void clearOpenFileError();

private:
  bool openFileModalRequested_ = false;
  bool openAboutPopup_ = false;
  bool openLicensesPopup_ = false;
  std::array<char, 4096> openFilePathBuffer_{};
  std::string openFileError_;
  std::string editorNoticeText_;
};

}  // namespace donner::editor
