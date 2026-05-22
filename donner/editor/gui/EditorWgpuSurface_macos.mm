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

wgpu::Surface CreateWgpuSurfaceFromGlfwWindow(const wgpu::Instance& instance, GLFWwindow* window) {
  if (window == nullptr) {
    return {};
  }

  NSWindow* nswindow = glfwGetCocoaWindow(window);
  if (nswindow == nil) {
    return {};
  }

  NSView* view = [nswindow contentView];
  CAMetalLayer* metalLayer = [CAMetalLayer layer];
  [view setWantsLayer:YES];
  [view setLayer:metalLayer];

  wgpu::SurfaceSourceMetalLayer source(wgpu::Default);
  source.layer = (__bridge void*)metalLayer;

  wgpu::SurfaceDescriptor desc(wgpu::Default);
  desc.nextInChain = &source.chain;

  return instance.createSurface(desc);
}

}  // namespace donner::editor::gui
