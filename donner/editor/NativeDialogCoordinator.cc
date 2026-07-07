#include "donner/editor/NativeDialogCoordinator.h"

#include <filesystem>

#include "donner/editor/NativeFileDialog.h"

namespace donner::editor {

bool NativeDialogCoordinator::available() const {
  return NativeFileDialogAvailable();
}

std::optional<std::string> NativeDialogCoordinator::openFile(
    GLFWwindow* window, const std::optional<std::string>& currentFilePath) {
  NativeOpenDialogRequest request;
  request.title = "Open SVG";
  request.defaultDirectory = state_.defaultDirectory(currentFilePath);
  request.filters = SvgFileDialogFilters();

  std::optional<std::string> chosen = ShowNativeOpenFileDialog(window, request);
  if (chosen.has_value()) {
    recordChosen(*chosen);
  }
  return chosen;
}

std::optional<std::string> NativeDialogCoordinator::saveFile(
    GLFWwindow* window, const std::optional<std::string>& suggestedPath) {
  NativeSaveDialogRequest request;
  request.title = "Save SVG";
  request.defaultDirectory = state_.defaultDirectory(suggestedPath);
  request.filters = SvgFileDialogFilters();
  if (suggestedPath.has_value() && !suggestedPath->empty()) {
    request.defaultName = std::filesystem::path(*suggestedPath).filename().string();
  } else {
    request.defaultName = "untitled.svg";
  }

  std::optional<std::string> chosen = ShowNativeSaveFileDialog(window, request);
  if (chosen.has_value()) {
    recordChosen(*chosen);
  }
  return chosen;
}

void NativeDialogCoordinator::recordChosen(const std::string& path) {
  state_.noteChosenPath(path);
  NoteNativeRecentDocument(path);
}

}  // namespace donner::editor
