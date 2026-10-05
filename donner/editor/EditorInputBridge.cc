#include "donner/editor/EditorInputBridge.h"

#ifdef __EMSCRIPTEN__
#include "donner/editor/WholeAppWorkerBridge.h"
#endif

#include <utility>

#include "GLFW/glfw3.h"
#include "donner/editor/PinchEventMonitor.h"

namespace donner::editor {

namespace {

#ifdef __EMSCRIPTEN__
// The zoom-modifier shadow is a `document`-scoped capture listener, so it lives
// in the shared-memory mirror that `whole_app_worker::Install()` sets up rather
// than on the app thread.
int WasmWheelZoomModifierHeld() {
  return whole_app_worker::ZoomModifierHeld() ? 1 : 0;
}
void RecordWasmScrollDebug(int zoomModifierHeld, double xoffset, double yoffset, int phys) {
  whole_app_worker::RecordScrollDebug(zoomModifierHeld != 0, xoffset, yoffset, phys != 0);
}
#endif

[[nodiscard]] bool IsPhysicalZoomKeyHeld(GLFWwindow* window) {
  return glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
}

[[nodiscard]] bool IsZoomModifierHeld(GLFWwindow* window) {
  const bool keyHeld = IsPhysicalZoomKeyHeld(window);
#ifdef __EMSCRIPTEN__
  return keyHeld || WasmWheelZoomModifierHeld() != 0;
#else
  return keyHeld;
#endif
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

  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window, &cursorX, &cursorY);

  // Forward to the previous callback (the ImGui backend, which feeds
  // io.MouseWheel and scrolls whatever UI window is hovered) only when the
  // canvas does NOT own the event. Over the render pane the canvas consumes
  // the wheel as pan/zoom, and forwarding it too would also scroll the
  // surrounding UI panes.
  if (state->previousCallback != nullptr &&
      !CanvasOwnsScrollEvent(state->canvasScrollCaptureRect, Vector2d(cursorX, cursorY))) {
    state->previousCallback(window, xoffset, yoffset);
  }

  const bool zoomModifierHeld = IsZoomModifierHeld(window);
  double effectiveYOffset = yoffset;
#ifdef __EMSCRIPTEN__
  // A ctrl-flagged wheel with no Ctrl/Cmd physically held is a
  // browser-synthesized trackpad pinch (Chromium: deltaY = -100*ln(scale),
  // Gecko: -100*magnification). Those need the desktop pinch calibration so
  // a pinch gesture matches zoom = 1 + magnification; a real ctrl+mouse-wheel
  // keeps the discrete per-notch step. See PinchZoomPolicy.h.
  if (zoomModifierHeld && !IsPhysicalZoomKeyHeld(window) && WasmWheelZoomModifierHeld() != 0) {
    effectiveYOffset = ApplyPinchScrollUnitGain(yoffset);
  }
  RecordWasmScrollDebug(zoomModifierHeld ? 1 : 0, xoffset, effectiveYOffset,
                        IsPhysicalZoomKeyHeld(window) ? 1 : 0);
#endif
  state->events.push_back(RenderPaneScrollEvent{
      .scrollDelta = Vector2d(xoffset, effectiveYOffset),
      .cursorScreen = Vector2d(cursorX, cursorY),
      .zoomModifierHeld = zoomModifierHeld,
  });
}

}  // namespace donner::editor
