/// @file
/// macOS (Cocoa/Metal) implementation of `CreateSurfaceFromGlfwWindow`.
///
/// wgpu-native's Metal backend expects a `CAMetalLayer`, not an `NSWindow`.
/// GLFW gives us an `NSWindow*` via `glfwGetCocoaWindow`; we install a
/// `CAMetalLayer` on its content view if one isn't already there, then hand
/// the layer pointer to webgpu via `SurfaceSourceMetalLayer`.

#include "examples/geode_embed_surface.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

namespace donner::example {

wgpu::Surface CreateSurfaceFromGlfwWindow(const wgpu::Instance& instance, GLFWwindow* window) {
  if (window == nullptr) {
    return {};
  }

  NSWindow* nswindow = glfwGetCocoaWindow(window);
  if (nswindow == nil) {
    return {};
  }

  // Ensure the content view is layer-backed and owns a CAMetalLayer. GLFW
  // creates an NSOpenGLView-compatible NSView by default; promoting it to a
  // Metal-backed layer is a one-liner and matches the pattern used by Dawn's
  // `utils_metal.mm` and the wgpu-native examples.
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

}  // namespace donner::example
