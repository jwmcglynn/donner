#pragma once
/// @file

#include <optional>
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

  /// Publish the screen-pixel rect inside which the canvas owns raw scroll
  /// events (they are queued for the render pane and NOT forwarded to the
  /// ImGui backend, so scrolling the canvas never also scrolls the
  /// surrounding UI). Pass `std::nullopt` to disable capture, e.g. while a
  /// popup/modal is capturing input. The shell updates this every frame.
  void setCanvasScrollCaptureRect(std::optional<Box2d> rect) {
    pendingScrollEvents_.canvasScrollCaptureRect = rect;
  }

private:
  struct PendingScrollEvents {
    GLFWscrollfun previousCallback = nullptr;
    std::vector<RenderPaneScrollEvent> events;
    /// See `setCanvasScrollCaptureRect`.
    std::optional<Box2d> canvasScrollCaptureRect;
  };

  static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

  gui::EditorWindow& window_;
  PendingScrollEvents pendingScrollEvents_;
};

}  // namespace donner::editor
