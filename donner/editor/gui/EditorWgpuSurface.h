#pragma once
/// @file
/// Platform-native WebGPU surface creation for editor GLFW windows.

#include <webgpu/webgpu.hpp>

struct GLFWwindow;

namespace donner::editor::gui {

#ifdef __APPLE__
/// Attach a Core Animation Metal layer to the content view of \p window and return it.
///
/// The layer is the platform object a surface presents to, and is handed back as the opaque
/// pointer a native surface handle names rather than as a typed Cocoa object, so callers that
/// only pass it along need no Objective-C.
///
/// @param window GLFW window whose content view receives the layer.
/// @return The layer, or null when \p window has no native Cocoa window.
[[nodiscard]] void* AttachMetalLayerToGlfwWindow(GLFWwindow* window);
#else
/// Create a WebGPU surface backed by the platform-native handle of \p window.
///
/// @param instance WebGPU instance used to create the surface.
/// @param window GLFW window that owns the native platform window.
/// @return A valid surface, or an invalid surface when native handle extraction fails.
[[nodiscard]] wgpu::Surface CreateWgpuSurfaceFromGlfwWindow(const wgpu::Instance& instance,
                                                            GLFWwindow* window);
#endif

}  // namespace donner::editor::gui
