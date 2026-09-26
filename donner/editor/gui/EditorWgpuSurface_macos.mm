/// @file
/// macOS (Cocoa/Metal) implementation of `AttachMetalLayerToGlfwWindow`.

#include "donner/editor/gui/EditorWgpuSurface.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#define GLFW_EXPOSE_NATIVE_COCOA
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

namespace donner::editor::gui {

void UpdateMetalLayerBackingScale(GLFWwindow* window) {
  if (window == nullptr) {
    return;
  }
  NSWindow* nswindow = glfwGetCocoaWindow(window);
  if (nswindow == nil) {
    return;
  }
  CALayer* layer = [nswindow contentView].layer;
  if (![layer isKindOfClass:[CAMetalLayer class]]) {
    return;
  }
  const CGFloat scale = nswindow.backingScaleFactor;
  if (scale > 0.0 && layer.contentsScale != scale) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contentsScale = scale;
    [CATransaction commit];
  }
}

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
  // GLFW sizes the framebuffer in backing pixels. A hosted layer starts at scale 1 even on a
  // Retina window, so synchronize it before the first surface configuration.
  UpdateMetalLayerBackingScale(window);
  return (__bridge void*)metalLayer;
}

}  // namespace donner::editor::gui
