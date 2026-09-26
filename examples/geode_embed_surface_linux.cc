/// @file
/// Linux GLFW surface-aware Vulkan selection for the native Geode embed example.

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "donner/gpu/vulkan/VulkanDevice.h"
#include "examples/geode_embed_surface.h"

#define GLFW_INCLUDE_VULKAN
extern "C" {
#include "GLFW/glfw3.h"
}

namespace donner::example {

namespace {
RetainedVulkanWindow* gRetainedWindows = nullptr;
}

NativeEmbedSurface PrepareNativeEmbedSurface(GLFWwindow* window) {
  NativeEmbedSurface selected;
  if (window == nullptr || glfwVulkanSupported() != GLFW_TRUE) {
    return selected;
  }
  uint32_t count = 0;
  const char** required = glfwGetRequiredInstanceExtensions(&count);
  if (required == nullptr || count == 0) {
    return selected;
  }
  for (uint32_t index = 0; index < count; ++index) {
    if (required[index] == nullptr) {
      return selected;
    }
  }

  selected.quarantine = std::make_unique<RetainedVulkanWindow>();
  auto loss = std::make_shared<gpu::DeviceLostState>();
  std::unique_ptr<gpu::vulkan::VulkanPresentationProbe> probe =
      gpu::vulkan::VulkanDevice::CreatePresentationProbe(
          std::span<const char* const>(required, count), loss);
  if (probe == nullptr) {
    return selected;
  }

  VkSurfaceKHR native = VK_NULL_HANDLE;
  const VkResult created = glfwCreateWindowSurface(static_cast<VkInstance>(probe->nativeInstance()),
                                                   window, nullptr, &native);
  if (created != VK_SUCCESS || native == VK_NULL_HANDLE) {
    if (native != VK_NULL_HANDLE) {
      uint64_t handle = 0;
      static_assert(sizeof(native) == sizeof(handle));
      std::memcpy(&handle, &native, sizeof(handle));
      probe->destroyExternalSurface(handle);
    }
    return selected;
  }
  static_assert(sizeof(native) == sizeof(selected.externalSurface));
  std::memcpy(&selected.externalSurface, &native, sizeof(native));

  selected.vulkanRoot =
      gpu::vulkan::VulkanDevice::CompletePresentationRoot(*probe, selected.externalSurface);
  if (selected.vulkanRoot == nullptr) {
    if (probe->nativeInstance() != nullptr) {
      probe->destroyExternalSurface(selected.externalSurface);
      selected.externalSurface = 0;
    }
    return selected;
  }
  selected.retirement = selected.vulkanRoot->registerExternalSurface(selected.externalSurface);
  if (selected.retirement == nullptr) {
    return selected;
  }
  const std::optional<gpu::TextureFormat> format =
      selected.vulkanRoot->preferredExternalSurfaceFormat(selected.externalSurface);
  if (!format.has_value()) {
    return selected;
  }
  selected.format = *format;
  selected.root = geode::AdoptNativeVulkanRoot(selected.vulkanRoot, loss);
  selected.native.kind = gpu::NativeSurfaceKind::EmbedderSurface;
  selected.native.window = selected.externalSurface;
  return selected;
}

bool RetireNativeEmbedSurface(NativeEmbedSurface& selected, GLFWwindow* window) {
  if (selected.externalSurface == 0) {
    return true;
  }
  const bool released =
      selected.vulkanRoot != nullptr &&
      selected.vulkanRoot->destroyExternalSurface(selected.externalSurface, selected.retirement);
  if (!released) {
    RetainedVulkanWindow* retained = selected.quarantine.release();
    retained->window = window;
    retained->surface = selected.externalSurface;
    retained->root = std::move(selected.vulkanRoot);
    retained->retirement = std::move(selected.retirement);
    retained->next = gRetainedWindows;
    gRetainedWindows = retained;
  }
  selected.externalSurface = 0;
  return released;
}

}  // namespace donner::example
