#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanLoader - runtime loading of the Vulkan loader library and its
/// entry points.

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

#include "donner/gpu/GpuResult.h"

namespace donner::gpu::vulkan {

/**
 * Vulkan entry points this backend calls, resolved at runtime.
 *
 * Every member is null until the tier that provides it has been loaded, and each tier is
 * resolved through the mechanism the Vulkan specification defines for it. Nothing here is a
 * link-time symbol: the backend calls Vulkan exclusively through this table, so a build links
 * without any Vulkan library present and a run without a loader installed fails closed at
 * device creation instead of failing to start.
 *
 * Resolving each entry point at the narrowest tier that can provide it also lets the loader
 * hand back the implementation's own function for a specific device rather than a dispatch
 * thunk that has to re-resolve the device on every call, which is why the draw-time entry
 * points are device-level.
 */
struct VulkanApi {
  /// @name Loader entry point
  /// @{
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;  //!< Resolves every other function.
  /// @}

  /// @name Global entry points, resolved with a null instance
  /// @{
  PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
  PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
  PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
  PFN_vkCreateInstance vkCreateInstance = nullptr;
  /// @}

  /// @name Instance-level entry points
  /// @{
  PFN_vkDestroyInstance vkDestroyInstance = nullptr;
  PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkCreateDevice vkCreateDevice = nullptr;
  PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
  /// Optional: present only when the debug-utils extension is enabled. Null otherwise.
  PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
  /// Optional, paired with \ref vkCreateDebugUtilsMessengerEXT.
  PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
  /// @}

  /// @name Device-level entry points
  /// @{
  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCreateImageView vkCreateImageView = nullptr;
  PFN_vkDestroyImageView vkDestroyImageView = nullptr;
  PFN_vkCreateSampler vkCreateSampler = nullptr;
  PFN_vkDestroySampler vkDestroySampler = nullptr;
  PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
  PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
  PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
  PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
  PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
  PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
  PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
  PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
  PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
  PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
  PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
  PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
  PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkGetFenceStatus vkGetFenceStatus = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;
  PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
  PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
  PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
  PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
  PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
  PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
  PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
  PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
  PFN_vkCmdDraw vkCmdDraw = nullptr;
  PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
  PFN_vkCmdDispatch vkCmdDispatch = nullptr;
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyBuffer vkCmdCopyBuffer = nullptr;
  PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
  PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
  /// @}

  /// @name Presentation entry points
  ///
  /// Null unless the device was created with presentation support: they come from extensions
  /// (VK_KHR_surface and its platform companions at instance level, VK_KHR_swapchain at device
  /// level), so a headless instance never resolves them and a backend without presentation
  /// never calls one.
  /// @{
  /// Core rather than an extension entry point, resolved here for the same reason as the two
  /// semaphore functions below: only presentation needs it, so the headless path is unchanged.
  PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
  PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
  /// Optional even among these: present only when VK_EXT_headless_surface was enabled.
  PFN_vkCreateHeadlessSurfaceEXT vkCreateHeadlessSurfaceEXT = nullptr;
  /// Core entry points rather than extension ones, resolved here because presentation is the
  /// only thing in this backend that needs a semaphore; keeping them in this group leaves the
  /// headless path resolving exactly what it did before.
  PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
  PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
  PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
  PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
  PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
  PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
  PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
  /// @}
};

/**
 * Owns the dynamically opened Vulkan loader library and the \ref VulkanApi resolved from it.
 *
 * The library stays open for as long as this object lives, which must outlive every Vulkan
 * object created through it: closing the loader invalidates the code behind every resolved
 * entry point. Devices therefore hold this by shared pointer.
 *
 * All three loading steps fail closed with a \ref GpuError naming exactly what was missing, so
 * an embedder on a machine with no Vulkan installed gets a clean unsupported result rather than
 * a crash.
 */
class VulkanLoader {
public:
  /**
   * Opens the platform's Vulkan loader and resolves the global entry points.
   *
   * The loader is opened by its stable versioned name first, since that is the name a
   * deployed runtime is guaranteed to provide; the unversioned development symlink is a
   * fallback for environments that ship only it.
   */
  static Result<std::shared_ptr<VulkanLoader>> Open();

  /// Closes the loader library.
  ~VulkanLoader();

  VulkanLoader(const VulkanLoader&) = delete;
  VulkanLoader& operator=(const VulkanLoader&) = delete;
  VulkanLoader(VulkanLoader&&) = delete;
  VulkanLoader& operator=(VulkanLoader&&) = delete;

  /// The resolved entry points. Members become non-null as each tier is loaded.
  const VulkanApi& api() const { return api_; }

  /**
   * Resolves the instance-level entry points against \p instance.
   *
   * @param instance Instance the entry points are resolved for.
   * @param debugUtilsEnabled True when the debug-utils extension was enabled on \p instance, in
   *   which case its two entry points are resolved as well; they stay null otherwise.
   */
  Status loadInstance(VkInstance instance, bool debugUtilsEnabled);

  /**
   * Resolves the device-level entry points against \p device, bypassing the loader's
   * instance-level dispatch for everything recorded per draw.
   *
   * @param device Device the entry points are resolved for.
   */
  Status loadDevice(VkDevice device);

  /**
   * Resolves the instance-level presentation entry points against \p instance.
   *
   * Separate from \ref loadInstance because presentation is opt in: a headless instance has no
   * surface extension to resolve them from, and leaving them null is what makes a backend
   * without presentation refuse rather than call into nothing.
   *
   * @param instance Instance the entry points are resolved for; must have been created with
   *   VK_KHR_surface enabled.
   * @param headlessSurfaceEnabled True when VK_EXT_headless_surface was enabled on \p instance,
   *   in which case its entry point is resolved as well; it stays null otherwise.
   */
  Status loadPresentationInstance(VkInstance instance, bool headlessSurfaceEnabled);

  /**
   * Resolves the device-level swapchain entry points against \p device.
   *
   * @param device Device the entry points are resolved for; must have been created with
   *   VK_KHR_swapchain enabled.
   */
  Status loadPresentationDevice(VkDevice device);

private:
  /// Constructs an empty loader holding \p library. @param library Opened loader handle.
  explicit VulkanLoader(void* library) : library_(library) {}

  void* library_ = nullptr;
  VulkanApi api_;
};

/// Returns the name of a VkResult for diagnostics (the common codes; others print
/// numerically). Shared so every failure message in this backend names the result the same way.
/// @param result Result to format.
std::string VkResultToString(VkResult result);

}  // namespace donner::gpu::vulkan
