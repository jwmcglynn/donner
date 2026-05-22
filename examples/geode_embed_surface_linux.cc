/// @file
/// Linux (X11) implementation of `CreateSurfaceFromGlfwWindow`.

#include "examples/geode_embed_surface.h"

#define GLFW_EXPOSE_NATIVE_X11
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

#ifdef None
#undef None
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#ifdef Status
#undef Status
#endif

namespace donner::example {

wgpu::Surface CreateSurfaceFromGlfwWindow(const wgpu::Instance& instance, GLFWwindow* window) {
  if (window == nullptr) {
    return {};
  }

  Display* display = glfwGetX11Display();
  const Window xwindow = glfwGetX11Window(window);
  if (display == nullptr || xwindow == 0) {
    return {};
  }

  wgpu::SurfaceSourceXlibWindow source(wgpu::Default);
  source.display = display;
  source.window = static_cast<uint64_t>(xwindow);

  wgpu::SurfaceDescriptor desc(wgpu::Default);
  desc.nextInChain = &source.chain;

  return instance.createSurface(desc);
}

}  // namespace donner::example
