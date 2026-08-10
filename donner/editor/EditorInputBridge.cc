#include "donner/editor/EditorInputBridge.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <utility>

#include "GLFW/glfw3.h"
#include "donner/editor/PinchEventMonitor.h"

namespace donner::editor {

namespace {

#ifdef __EMSCRIPTEN__
// clang-format off
EM_JS(void, InstallWasmWheelModifierCapture, (), {
  const canvas = document.getElementById("canvas");
  if (!canvas) {
    return;
  }

  if (canvas['__donnerWheelModifierCapture']) {
    canvas['__donnerWheelModifierCapture']['state']['zoomModifierHeld'] = false;
    return;
  }

  const state = {
    'zoomModifierHeld': false,
  };
  const handler = function(event) {
    state.zoomModifierHeld = !!(event.ctrlKey || event.metaKey);
  };
  // macOS drops the keyup when Cmd is held across a focus change; never let a
  // stale modifier shadow classify ordinary scrolls as zoom after a blur.
  const clear = function() {
    state.zoomModifierHeld = false;
  };
  window.addEventListener("blur", clear);
  document.addEventListener("visibilitychange", clear);

  canvas['__donnerWheelModifierCapture'] = {
    'state': state,
    'handler': handler,
  };
  canvas.addEventListener("wheel", handler, {capture: true, passive: false});
});

EM_JS(void, RemoveWasmWheelModifierCapture, (), {
  const canvas = document.getElementById("canvas");
  const capture = canvas && canvas['__donnerWheelModifierCapture'];
  if (!capture) {
    return;
  }

  canvas.removeEventListener("wheel", capture['handler'], true);
  delete canvas['__donnerWheelModifierCapture'];
});

EM_JS(int, WasmWheelZoomModifierHeld, (), {
  const canvas = document.getElementById("canvas");
  const capture = canvas && canvas['__donnerWheelModifierCapture'];
  return capture && capture['state'] && capture['state']['zoomModifierHeld'] ? 1 : 0;
});

EM_JS(void, RecordWasmScrollDebug, (int zoomModifierHeld, double xoffset, double yoffset, int phys, int dom), {
  const previous = window['__donnerLastScrollEvent'];
  window['__donnerLastScrollEvent'] = {
    'zoomModifierHeld': !!zoomModifierHeld,
    'phys': phys,
    'dom': dom,
    'xoffset': xoffset,
    'yoffset': yoffset,
    'count': ((previous && previous['count']) || 0) + 1,
  };
});
// clang-format on
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
#ifdef __EMSCRIPTEN__
  InstallWasmWheelModifierCapture();
#endif
  (void)InstallPinchEventMonitor(window_.rawHandle(), &pendingScrollEvents_.events, wheelZoomStep);
}

EditorInputBridge::~EditorInputBridge() {
#ifdef __EMSCRIPTEN__
  RemoveWasmWheelModifierCapture();
#endif
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
    const double maxUnits = MaxPinchScrollUnitsPerEvent();
    effectiveYOffset = std::clamp(yoffset * PinchScrollUnitGain(), -maxUnits, maxUnits);
  }
  RecordWasmScrollDebug(zoomModifierHeld ? 1 : 0, xoffset, effectiveYOffset,
                        IsPhysicalZoomKeyHeld(window) ? 1 : 0, WasmWheelZoomModifierHeld());
#endif
  state->events.push_back(RenderPaneScrollEvent{
      .scrollDelta = Vector2d(xoffset, effectiveYOffset),
      .cursorScreen = Vector2d(cursorX, cursorY),
      .zoomModifierHeld = zoomModifierHeld,
  });
}

}  // namespace donner::editor
