/// @file
/// Linux (X11) implementation of `CreateSurfaceFromGlfwWindow`.
///
/// Two header-ordering hazards to be aware of here:
///
/// 1. `<X11/Xlib.h>` — transitively pulled in by GLFW's native header when
///    `GLFW_EXPOSE_NATIVE_X11` is defined — `#define`s `None`, `True`,
///    `False`, and `Status`. `Status` collides with the `wgpu::Status`
///    enum class declared in `<webgpu/webgpu.hpp>`. We include webgpu.hpp
///    first so the enum is fully declared before the macro is defined,
///    then `#undef` the offending macros to keep any later includes (and
///    inline code below) unaffected.
///
/// 2. No donner headers are included in this TU. That keeps the X11 macro
///    fallout fully contained — consumers interact with this file via the
///    macro-free `geode_embed_surface.h` facade.

#include "examples/geode_embed_surface.h"

#define GLFW_EXPOSE_NATIVE_X11
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

// Xlib macros that collide with C++ identifiers used elsewhere. Dropping
// them after Xlib is fully parsed is safe because this TU does not call
// any Xlib function that needs the symbolic constants.
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

  // GLFW is built with `_GLFW_X11`; these return the Xlib handles directly.
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
