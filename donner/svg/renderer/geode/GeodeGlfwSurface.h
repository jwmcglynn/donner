#pragma once
/// @file
/// Platform-native WebGPU surface creation for GLFW windows.

#include <webgpu/webgpu.hpp>

struct GLFWwindow;

namespace donner::geode {

/// Create a WebGPU surface backed by the platform-native handle of \p window.
///
/// Returns an invalid surface when the platform is unsupported or native handle extraction fails.
[[nodiscard]] wgpu::Surface CreateSurfaceFromGlfwWindow(const wgpu::Instance& instance,
                                                        GLFWwindow* window);

}  // namespace donner::geode
