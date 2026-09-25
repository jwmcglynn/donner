#include "donner/editor/NativeFileDialog.h"

// Non-macOS fallback: native file dialogs are unavailable, so callers keep
// using the in-editor ImGui path-entry modal.

namespace donner::editor {

bool NativeFileDialogAvailable() {
  return false;
}

std::optional<std::string> ShowNativeOpenFileDialog(GLFWwindow* parent,
                                                    const NativeOpenDialogRequest& request) {
  (void)parent;
  (void)request;
  return std::nullopt;
}

std::optional<std::string> ShowNativeSaveFileDialog(GLFWwindow* parent,
                                                    const NativeSaveDialogRequest& request) {
  (void)parent;
  (void)request;
  return std::nullopt;
}

void ShowNativeFileDialogError(GLFWwindow* parent, const std::string& title,
                               const std::string& message) {
  (void)parent;
  (void)title;
  (void)message;
}

void NoteNativeRecentDocument(const std::string& path) {
  (void)path;
}

}  // namespace donner::editor
