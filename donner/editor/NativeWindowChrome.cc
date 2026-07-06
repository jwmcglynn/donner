#include "donner/editor/NativeWindowChrome.h"

#include <filesystem>

namespace donner::editor {

std::string ComposeWindowTitle(const WindowChromeState& state, bool showEditedDotInText) {
  std::string title;
  if (showEditedDotInText && state.edited) {
    title += "\xE2\x97\x8F ";  // U+25CF BLACK CIRCLE.
  }
  if (state.filePath.has_value() && !state.filePath->empty()) {
    title += std::filesystem::path(*state.filePath).filename().string();
  } else {
    title += "untitled";
  }
  title += " - Donner SVG Editor";
  return title;
}

}  // namespace donner::editor
