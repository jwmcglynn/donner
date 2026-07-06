#include "donner/editor/FileDialogState.h"

#include <filesystem>
#include <utility>

namespace donner::editor {

std::vector<FileDialogFilter> SvgFileDialogFilters() {
  return {FileDialogFilter{.description = "SVG Image", .extensions = {"svg"}}};
}

FileDialogState::FileDialogState(std::size_t maxRecents) : maxRecents_(maxRecents) {}

std::optional<std::string> FileDialogState::defaultDirectory(
    const std::optional<std::string>& currentFilePath) const {
  if (lastDirectory_.has_value()) {
    return lastDirectory_;
  }
  if (currentFilePath.has_value() && !currentFilePath->empty()) {
    const std::filesystem::path parent = std::filesystem::path(*currentFilePath).parent_path();
    if (!parent.empty()) {
      return parent.string();
    }
  }
  return std::nullopt;
}

void FileDialogState::noteChosenPath(std::string_view path) {
  if (path.empty()) {
    return;
  }

  const std::filesystem::path fsPath{std::string(path)};
  const std::filesystem::path parent = fsPath.parent_path();
  if (!parent.empty()) {
    lastDirectory_ = parent.string();
  }

  const std::string pathStr(path);
  // Move-to-front with de-duplication: an already-listed file jumps back to
  // the top rather than appearing twice.
  for (auto it = recentFiles_.begin(); it != recentFiles_.end(); ++it) {
    if (*it == pathStr) {
      recentFiles_.erase(it);
      break;
    }
  }
  recentFiles_.insert(recentFiles_.begin(), pathStr);
  if (recentFiles_.size() > maxRecents_) {
    recentFiles_.resize(maxRecents_);
  }
}

}  // namespace donner::editor
