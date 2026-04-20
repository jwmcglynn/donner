#include "donner/editor/EditorInputBridge.h"

#include <utility>

#include "GLFW/glfw3.h"
#include "donner/editor/PinchEventMonitor.h"

namespace donner::editor {

namespace {

[[nodiscard]] bool IsZoomModifierHeld(GLFWwindow* window) {
  return glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
}

}  // namespace

EditorInputBridge::EditorInputBridge(gui::EditorWindow& window, double wheelZoomStep)
    : window_(window) {
  window_.setUserPointer(&pendingScrollEvents_);
  pendingScrollEvents_.previousCallback =
      window_.setScrollCallback(&EditorInputBridge::ScrollCallback);
  (void)InstallPinchEventMonitor(window_.rawHandle(), &pendingScrollEvents_.events, wheelZoomStep);
}

EditorInputBridge::~EditorInputBridge() {
  if (window_.rawHandle() != nullptr) {
    std::ignore = window_.setScrollCallback(pendingScrollEvents_.previousCallback);
    window_.setUserPointer(nullptr);
  }
}

void EditorInputBridge::clear() {
  pendingScrollEvents_.events.clear();
}

void EditorInputBridge::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
  auto* state = static_cast<PendingScrollEvents*>(glfwGetWindowUserPointer(window));
  if (state == nullptr) {
    return;
  }

  if (state->previousCallback != nullptr) {
    state->previousCallback(window, xoffset, yoffset);
  }

  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window, &cursorX, &cursorY);
  state->events.push_back(RenderPaneScrollEvent{
      .scrollDelta = Vector2d(xoffset, yoffset),
      .cursorScreen = Vector2d(cursorX, cursorY),
      .zoomModifierHeld = IsZoomModifierHeld(window),
  });
}

}  // namespace donner::editor
