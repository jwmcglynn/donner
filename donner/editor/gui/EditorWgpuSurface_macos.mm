/// @file
/// macOS (Cocoa/Metal) implementation of `CreateWgpuSurfaceFromGlfwWindow`.

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

wgpu::Surface CreateWgpuSurfaceFromGlfwWindow(const wgpu::Instance& instance, GLFWwindow* window) {
  void* metalLayer = AttachMetalLayerToGlfwWindow(window);
  if (metalLayer == nullptr) {
    return {};
  }

  wgpu::SurfaceSourceMetalLayer source(wgpu::Default);
  source.layer = metalLayer;

  wgpu::SurfaceDescriptor desc(wgpu::Default);
  desc.nextInChain = &source.chain;

  return instance.createSurface(desc);
}

}  // namespace donner::editor::gui
