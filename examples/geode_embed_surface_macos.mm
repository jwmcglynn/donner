/// @file
/// macOS GLFW Metal layer setup for the native Geode embed example.

#include "examples/geode_embed_surface.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

namespace donner::example {

NativeEmbedSurface PrepareNativeEmbedSurface(GLFWwindow* window) {
  NativeEmbedSurface selected;
  if (window == nullptr) {
    return selected;
  }

  NSWindow* nswindow = glfwGetCocoaWindow(window);
  if (nswindow == nil) {
    return selected;
  }

  NSView* view = [nswindow contentView];
  CAMetalLayer* metalLayer = [CAMetalLayer layer];
  [view setWantsLayer:YES];
  [view setLayer:metalLayer];

  selected.native.kind = gpu::NativeSurfaceKind::MetalLayer;
  selected.native.display = (__bridge void*)metalLayer;
  selected.format = gpu::TextureFormat::BGRA8Unorm;
  geode::GpuRootSelection selection;
  selection.label = "GeodeEmbedMetal";
  selected.root = geode::SelectGpuRoot(selection);
  return selected;
}

bool RetireNativeEmbedSurface(NativeEmbedSurface& /*surface*/, GLFWwindow* /*window*/) {
  return true;
}

}  // namespace donner::example
