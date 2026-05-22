#pragma once
/// @file
/// Platform-native WebGPU surface creation for editor GLFW windows.

#include <webgpu/webgpu.hpp>

struct GLFWwindow;

namespace donner::editor::gui {

/// Create a WebGPU surface backed by the platform-native handle of \p window.
///
/// @param instance WebGPU instance used to create the surface.
/// @param window GLFW window that owns the native platform window.
/// @return A valid surface, or an invalid surface when native handle extraction fails.
[[nodiscard]] wgpu::Surface CreateWgpuSurfaceFromGlfwWindow(const wgpu::Instance& instance,
                                                            GLFWwindow* window);

}  // namespace donner::editor::gui
