#pragma once
/// @file
/// Native window surface prepared before Geode's device and pipelines are created.

#include <cstdint>
#include <memory>

#include "donner/gpu/Descriptors.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"

struct GLFWwindow;

namespace donner::gpu::vulkan {
class VulkanSharedRoot;
class VulkanSurfaceRetirement;
}  // namespace donner::gpu::vulkan

namespace donner::example {

#if defined(__linux__)
/// Preallocated so an unproven Vulkan surface can retain its window and root without allocating
/// at the point of failure. The process owns quarantined records until exit.
struct RetainedVulkanWindow {
  RetainedVulkanWindow* next = nullptr;
  GLFWwindow* window = nullptr;
  uint64_t surface = 0;
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> root;
  std::shared_ptr<gpu::vulkan::VulkanSurfaceRetirement> retirement;
};
#endif

/// Platform object and selected root for the example's runtime surface.
/// The GLFW window must outlive this object and its runtime surface.
struct NativeEmbedSurface {
  std::shared_ptr<geode::GeodeGpuRoot> root;
  gpu::NativeSurfaceHandle native;
  gpu::TextureFormat format = gpu::TextureFormat::BGRA8Unorm;
#if defined(__linux__)
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot;
  std::shared_ptr<gpu::vulkan::VulkanSurfaceRetirement> retirement;
  uint64_t externalSurface = 0;
  std::unique_ptr<RetainedVulkanWindow> quarantine;
#endif
};

/// Prepare a native window surface and select a root that can present to it.
/// A null root means preparation failed; call RetireNativeEmbedSurface even then.
[[nodiscard]] NativeEmbedSurface PrepareNativeEmbedSurface(GLFWwindow* window);

/// Release the embedder surface after the runtime surface and Geode context are destroyed.
/// False means the driver could not prove retirement; the helper retains the GLFW window and
/// native root until process exit, so the caller must not destroy the window or terminate GLFW.
[[nodiscard]] bool RetireNativeEmbedSurface(NativeEmbedSurface& surface, GLFWwindow* window);

}  // namespace donner::example
