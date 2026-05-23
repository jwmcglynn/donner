#include "donner/editor/EditorInputBridge.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

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

  if (canvas.__donnerWheelModifierCapture) {
    canvas.__donnerWheelModifierCapture.state.zoomModifierHeld = false;
    return;
  }

  const state = {
    zoomModifierHeld: false,
  };
  const handler = function(event) {
    state.zoomModifierHeld = !!(event.ctrlKey || event.metaKey);
  };

  canvas.__donnerWheelModifierCapture = {
    state: state,
    handler: handler,
  };
  canvas.addEventListener("wheel", handler, {capture: true, passive: false});
});

EM_JS(void, RemoveWasmWheelModifierCapture, (), {
  const canvas = document.getElementById("canvas");
  const capture = canvas && canvas.__donnerWheelModifierCapture;
  if (!capture) {
    return;
  }

  canvas.removeEventListener("wheel", capture.handler, true);
  delete canvas.__donnerWheelModifierCapture;
});

EM_JS(int, WasmWheelZoomModifierHeld, (), {
  const canvas = document.getElementById("canvas");
  const capture = canvas && canvas.__donnerWheelModifierCapture;
  return capture && capture.state && capture.state.zoomModifierHeld ? 1 : 0;
});

EM_JS(void, RecordWasmScrollDebug, (int zoomModifierHeld, double xoffset, double yoffset), {
  window.__donnerLastScrollEvent = {
    zoomModifierHeld: !!zoomModifierHeld,
    xoffset: xoffset,
    yoffset: yoffset,
    count: ((window.__donnerLastScrollEvent && window.__donnerLastScrollEvent.count) || 0) + 1,
  };
});
// clang-format on
#endif

[[nodiscard]] bool IsZoomModifierHeld(GLFWwindow* window) {
  const bool keyHeld = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                       glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
                       glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
                       glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
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

  if (state->previousCallback != nullptr) {
    state->previousCallback(window, xoffset, yoffset);
  }

  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window, &cursorX, &cursorY);
  const bool zoomModifierHeld = IsZoomModifierHeld(window);
#ifdef __EMSCRIPTEN__
  RecordWasmScrollDebug(zoomModifierHeld ? 1 : 0, xoffset, yoffset);
#endif
  state->events.push_back(RenderPaneScrollEvent{
      .scrollDelta = Vector2d(xoffset, yoffset),
      .cursorScreen = Vector2d(cursorX, cursorY),
      .zoomModifierHeld = zoomModifierHeld,
  });
}

}  // namespace donner::editor
