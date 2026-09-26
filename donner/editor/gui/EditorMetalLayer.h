#pragma once
/// @file
/// Core Animation layer attachment for a native Metal editor window.

struct GLFWwindow;

namespace donner::editor::gui {

/// Attach a Metal layer to the content view of a live GLFW window.
/// The view retains the layer for the window's lifetime.
[[nodiscard]] void* AttachMetalLayerToGlfwWindow(GLFWwindow* window);

}  // namespace donner::editor::gui
