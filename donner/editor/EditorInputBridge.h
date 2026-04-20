#pragma once
/// @file

#include <vector>

#include "donner/editor/RenderPaneGesture.h"
#include "donner/editor/gui/EditorWindow.h"

namespace donner::editor {

/**
 * Wraps the editor's raw GLFW scroll callback + native pinch monitor plumbing so the advanced
 * editor shell can consume queued gesture events without owning callback lifecycle directly.
 */
class EditorInputBridge {
public:
  EditorInputBridge(gui::EditorWindow& window, double wheelZoomStep);
  ~EditorInputBridge();

  EditorInputBridge(const EditorInputBridge&) = delete;
  EditorInputBridge& operator=(const EditorInputBridge&) = delete;

  /// Returns the queued render-pane scroll events captured since the previous frame.
  [[nodiscard]] std::vector<RenderPaneScrollEvent>& events() { return pendingScrollEvents_.events; }

  /// Clears the queued render-pane scroll events.
  void clear();

private:
  struct PendingScrollEvents {
    GLFWscrollfun previousCallback = nullptr;
    std::vector<RenderPaneScrollEvent> events;
  };

  static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

  gui::EditorWindow& window_;
  PendingScrollEvents pendingScrollEvents_;
};

}  // namespace donner::editor
