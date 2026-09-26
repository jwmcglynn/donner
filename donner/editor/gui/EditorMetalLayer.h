#pragma once
/// @file
/// Core Animation layer attachment for a native Metal editor window.

struct GLFWwindow;

namespace donner::editor::gui {

/// Attach a Metal layer to the content view of a live GLFW window.
/// The view retains the layer for the window's lifetime.
[[nodiscard]] void* AttachMetalLayerToGlfwWindow(GLFWwindow* window);

/// Keep the layer's point-to-pixel scale aligned with the Cocoa window after a backing-scale
/// change, such as moving between Retina and standard-density displays.
void UpdateMetalLayerBackingScale(GLFWwindow* window);

}  // namespace donner::editor::gui
