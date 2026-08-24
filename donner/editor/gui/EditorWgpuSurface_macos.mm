/// @file
/// macOS (Cocoa/Metal) implementation of `AttachMetalLayerToGlfwWindow`.

#include "donner/editor/gui/EditorWgpuSurface.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

namespace donner::editor::gui {

void* AttachMetalLayerToGlfwWindow(GLFWwindow* window) {
  if (window == nullptr) {
    return nullptr;
  }

  NSWindow* nswindow = glfwGetCocoaWindow(window);
  if (nswindow == nil) {
    return nullptr;
  }

  NSView* view = [nswindow contentView];
  CAMetalLayer* metalLayer = [CAMetalLayer layer];
  [view setWantsLayer:YES];
  // The view retains the layer, so the returned pointer stays valid for as long as the window's
  // content view does.
  [view setLayer:metalLayer];
  return (__bridge void*)metalLayer;
}

}  // namespace donner::editor::gui
