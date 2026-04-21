#pragma once
/// @file
/// Platform-native WebGPU surface creation helper for the geode_embed
/// example. The implementation lives in platform-specific translation units
/// (`geode_embed_surface_linux.cc`, `geode_embed_surface_macos.mm`) that
/// include native-window headers (X11, Cocoa) in isolation so their macro
/// pollution — `X11/Xlib.h` defining `None`, `True`, `False`, `Status` — never
/// reaches donner headers. The consumer (`geode_embed.cc`) only sees this
/// small facade.

#include <webgpu/webgpu.hpp>

struct GLFWwindow;

namespace donner::example {

/// Create a `wgpu::Surface` backed by the platform-native handle of `window`.
/// Returns an invalid `wgpu::Surface` when the platform is unsupported or
/// window-handle extraction failed.
wgpu::Surface CreateSurfaceFromGlfwWindow(const wgpu::Instance& instance, GLFWwindow* window);

}  // namespace donner::example
