#include "donner/editor/PinchEventMonitor.h"

#import <Cocoa/Cocoa.h>

#include <cmath>

#define GLFW_EXPOSE_NATIVE_COCOA
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

namespace donner::editor {

namespace {

constexpr double kEpsilonScroll = 1e-9;
id gPinchEventMonitor = nil;

}  // namespace

bool InstallPinchEventMonitor(GLFWwindow* window, std::vector<RenderPaneScrollEvent>* events,
                              double wheelZoomStep) {
  if (window == nullptr || events == nullptr) {
    return false;
  }

  if (gPinchEventMonitor != nil) {
    return true;
  }

  NSWindow* cocoaWindow = glfwGetCocoaWindow(window);
  if (cocoaWindow == nil) {
    return false;
  }

  gPinchEventMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
                                   handler:^NSEvent*(NSEvent* event) {
                                     if (event.window != cocoaWindow) {
                                       return event;
                                     }

                                     const double scrollDeltaY =
                                         PinchMagnificationToScrollDelta(event.magnification,
                                                                         wheelZoomStep);
                                     if (std::abs(scrollDeltaY) > kEpsilonScroll) {
                                       double cursorX = 0.0;
                                       double cursorY = 0.0;
                                       glfwGetCursorPos(window, &cursorX, &cursorY);
                                       events->push_back(RenderPaneScrollEvent{
                                           .scrollDelta = Vector2d(0.0, scrollDeltaY),
                                           .cursorScreen = Vector2d(cursorX, cursorY),
                                           .zoomModifierHeld = true,
                                       });
                                     }

                                     return nil;
                                   }];
  return gPinchEventMonitor != nil;
}

}  // namespace donner::editor
