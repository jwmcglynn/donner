/// @file
/// Vulkan backend implementation for \c donner::gpu::vulkan::VulkanDevice.
///
/// Compiles against the hermetic Vulkan-Headers module on every platform; links against the
/// Vulkan loader (and executes) only where one exists. Vulkan 1.1 core only: classic
/// VkRenderPass/VkFramebuffer, per-submission fences, and conservative validation-clean
/// synchronization (see the class comment in VulkanDevice.h).

#include "donner/gpu/vulkan/VulkanDevice.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/BufferMappingTable.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/vulkan/VulkanBufferAllocator.h"
#include "donner/gpu/vulkan/VulkanLoader.h"
#include "donner/gpu/vulkan/VulkanResourceState.h"
#include "donner/gpu/vulkan/VulkanSwapchain.h"

namespace donner::gpu::vulkan {

namespace {

/// Vulkan API version this backend targets (see the class comment: 1.1 core only).
constexpr uint32_t kTargetApiVersion = VK_API_VERSION_1_1;

/// Timeout for the synchronous internal texture-upload submission, in nanoseconds (60 s). A
/// stuck driver fails closed with an error instead of hanging the caller forever.
constexpr double kUploadFenceTimeoutSeconds = 60.0;

/// Bound for a host access waiting on the buffer's outstanding submission.
constexpr double kBusyBufferAccessTimeoutSeconds = 5.0;

/// Bound for each outstanding fence during device destruction.
constexpr uint64_t kTeardownFenceTimeoutNs = 5'000'000'000;

/// Only these submission errors guarantee that resources and synchronization are unchanged.
bool SubmissionWasRejected(VkResult result) {
  return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

/// Device loss from a completion operation has the same completion guarantee as success.
bool CompletionWasProven(VkResult result) {
  return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
}

/// Validation layer enabled when the loader enumerates it (CI installs it explicitly; plain
/// driver installs usually do not have it, and it is skipped silently then).
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

/// Builds a fail-closed \ref GpuError for a failed Vulkan call.
GpuError VkError(std::string_view what, VkResult result) {
  return GpuError{GpuErrorType::InvalidState,
                  std::format("{} failed with {}", what, VkResultToString(result))};
}

/// Ensures \p table covers \p slotIndex and stores \p value there. Slots are value-initialized
/// (empty optionals) until written.
template <typename T>
void SetSlot(std::vector<T>& table, uint32_t slotIndex, T value) {
  if (table.size() <= slotIndex) {
    table.resize(slotIndex + 1);
  }
  table[slotIndex] = std::move(value);
}

/// Returns a pointer to the record stored at \p slotIndex, or nullptr if the slot is empty.
template <typename Record>
Record* FindRecord(std::vector<std::optional<Record>>& table, uint32_t slotIndex) {
  if (slotIndex >= table.size() || !table[slotIndex].has_value()) {
    return nullptr;
  }
  return &table[slotIndex].value();
}

VkFormat ToVkFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated TextureFormat out of range");
  return VK_FORMAT_R8G8B8A8_UNORM;
}

VkFilter ToVkFilter(FilterMode mode) {
  switch (mode) {
    case FilterMode::Nearest: return VK_FILTER_NEAREST;
    case FilterMode::Linear: return VK_FILTER_LINEAR;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated FilterMode out of range");
  return VK_FILTER_NEAREST;
}

VkSamplerAddressMode ToVkAddressMode(AddressMode mode) {
  switch (mode) {
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated AddressMode out of range");
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkFormat ToVkVertexFormat(VertexFormat format) {
  switch (format) {
    case VertexFormat::Float32x2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::Uint32: return VK_FORMAT_R32_UINT;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexFormat out of range");
  return VK_FORMAT_R32G32_SFLOAT;
}

VkVertexInputRate ToVkInputRate(VertexStepMode mode) {
  switch (mode) {
    case VertexStepMode::Vertex: return VK_VERTEX_INPUT_RATE_VERTEX;
    case VertexStepMode::Instance: return VK_VERTEX_INPUT_RATE_INSTANCE;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexStepMode out of range");
  return VK_VERTEX_INPUT_RATE_VERTEX;
}

VkPrimitiveTopology ToVkTopology(PrimitiveTopology topology) {
  switch (topology) {
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated PrimitiveTopology out of range");
  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags ToVkCullMode(CullMode mode) {
  switch (mode) {
    case CullMode::None: return VK_CULL_MODE_NONE;
    case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated CullMode out of range");
  return VK_CULL_MODE_NONE;
}

VkBlendFactor ToVkBlendFactor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendFactor out of range");
  return VK_BLEND_FACTOR_ZERO;
}

VkBlendOp ToVkBlendOp(BlendOperation operation) {
  switch (operation) {
    case BlendOperation::Add: return VK_BLEND_OP_ADD;
    case BlendOperation::Max: return VK_BLEND_OP_MAX;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendOperation out of range");
  return VK_BLEND_OP_ADD;
}

VkColorComponentFlags ToVkColorWriteMask(ColorWriteMask mask) {
  VkColorComponentFlags result = 0;
  if (HasAllFlags(mask, ColorWriteMask::Red)) {
    result |= VK_COLOR_COMPONENT_R_BIT;
  }
  if (HasAllFlags(mask, ColorWriteMask::Green)) {
    result |= VK_COLOR_COMPONENT_G_BIT;
  }
  if (HasAllFlags(mask, ColorWriteMask::Blue)) {
    result |= VK_COLOR_COMPONENT_B_BIT;
  }
  if (HasAllFlags(mask, ColorWriteMask::Alpha)) {
    result |= VK_COLOR_COMPONENT_A_BIT;
  }
  return result;
}

VkAttachmentLoadOp ToVkLoadOp(LoadOp op) {
  switch (op) {
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated LoadOp out of range");
  return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp ToVkStoreOp(StoreOp op) {
  switch (op) {
    case StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
    case StoreOp::Discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated StoreOp out of range");
  return VK_ATTACHMENT_STORE_OP_STORE;
}

VkDescriptorType ToVkDescriptorType(BindingType type) {
  switch (type) {
    case BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::ReadOnlyStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingType::SampledTexture2dFloat:
    case BindingType::SampledTexture2dUnfilterableFloat: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::FilteringSampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingType::WriteOnlyStorageTexture2d: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BindingType out of range");
  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkShaderStageFlags ToVkShaderStages(ShaderStage visibility) {
  VkShaderStageFlags result = 0;
  if (HasAllFlags(visibility, ShaderStage::Vertex)) {
    result |= VK_SHADER_STAGE_VERTEX_BIT;
  }
  if (HasAllFlags(visibility, ShaderStage::Fragment)) {
    result |= VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  if (HasAllFlags(visibility, ShaderStage::Compute)) {
    result |= VK_SHADER_STAGE_COMPUTE_BIT;
  }
  return result;
}

VkBufferUsageFlags ToVkBufferUsage(BufferUsage usage) {
  VkBufferUsageFlags result = 0;
  if (HasAllFlags(usage, BufferUsage::Vertex)) {
    result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  }
  if (HasAllFlags(usage, BufferUsage::Index)) {
    result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  }
  if (HasAllFlags(usage, BufferUsage::Uniform)) {
    result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  }
  if (HasAllFlags(usage, BufferUsage::Storage)) {
    result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  }
  if (HasAllFlags(usage, BufferUsage::CopySrc)) {
    result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  }
  if (HasAllFlags(usage, BufferUsage::CopyDst)) {
    result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  }
  // MapRead has no VkBufferUsageFlags equivalent: readability is a memory property, and every
  // buffer in this slice is host-visible (see the class comment).
  return result;
}

VkImageUsageFlags ToVkImageUsage(TextureUsage usage) {
  VkImageUsageFlags result = 0;
  if (HasAllFlags(usage, TextureUsage::RenderAttachment)) {
    result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::Sampled)) {
    result |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::CopySrc)) {
    result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::CopyDst)) {
    result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::StorageBinding)) {
    result |= VK_IMAGE_USAGE_STORAGE_BIT;
  }
  return result;
}

/// Finds a memory type index compatible with \p typeBits carrying all \p required property
/// flags, or empty if none exists.
std::optional<uint32_t> FindMemoryType(const VkPhysicalDeviceMemoryProperties& properties,
                                       uint32_t typeBits, VkMemoryPropertyFlags required) {
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) != 0 &&
        (properties.memoryTypes[i].propertyFlags & required) == required) {
      return i;
    }
  }
  return std::nullopt;
}

/// The full color subresource range of a single-mip, single-layer 2D image.
VkImageSubresourceRange FullColorRange() {
  VkImageSubresourceRange range = {};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.baseMipLevel = 0;
  range.levelCount = 1;
  range.baseArrayLayer = 0;
  range.layerCount = 1;
  return range;
}

/// Latched first-failure state shared with the debug-utils messenger callback. Vulkan may
/// invoke the callback from any thread, so the flag is atomic and the message is mutex-guarded;
/// the owning device reads it from its single owning thread.
struct ErrorState {
  std::atomic<bool> hadError{false};  //!< True once any failure was recorded.
  std::mutex mutex;                   //!< Guards \ref message.
  std::string message;                //!< First recorded failure message.

  /// Latches \p newMessage (first failure wins). @param newMessage Failure description.
  void record(std::string newMessage) {
    hadError.store(true, std::memory_order_release);
    const std::lock_guard<std::mutex> lock(mutex);
    if (message.empty()) {
      message = std::move(newMessage);
    }
  }

  /// Returns the first recorded failure message (empty if none).
  std::string firstMessage() {
    const std::lock_guard<std::mutex> lock(mutex);
    return message;
  }
};

/// Debug-utils messenger callback: latches validation ERROR-severity messages into the device
/// error state, so tests observe validation findings as red assertions instead of log lines.
VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData) {
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 && userData != nullptr) {
    static_cast<ErrorState*>(userData)->record(std::format(
        "Vulkan validation error: {}",
        (callbackData != nullptr && callbackData->pMessage != nullptr) ? callbackData->pMessage
                                                                       : "(no message)"));
  }
  return VK_FALSE;
}

/// Records the image barrier \p params describes. The parameters come from the resource-state
/// model, which is where the decision of what to wait on and what to make visible lives; this
/// only turns that decision into the Vulkan call.
/// @param api Resolved device entry points.
/// @param commandBuffer Command buffer to record into.
/// @param image Image whose layout changes.
/// @param params Derived barrier parameters.
void RecordImageBarrier(const VulkanApi& api, VkCommandBuffer commandBuffer, VkImage image,
                        const ImageBarrierParams& params) {
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = params.srcAccess;
  barrier.dstAccessMask = params.dstAccess;
  barrier.oldLayout = params.oldLayout;
  barrier.newLayout = params.newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = FullColorRange();
  api.vkCmdPipelineBarrier(commandBuffer, params.srcStage, params.dstStage, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
}

/// Returns the Khronos validation layer if the loader enumerates it, otherwise an empty list.
/// @param api Resolved global entry points.
std::vector<const char*> EnumerateValidationLayer(const VulkanApi& api) {
  std::vector<const char*> enabledLayers;
  uint32_t layerCount = 0;
  if (api.vkEnumerateInstanceLayerProperties(&layerCount, nullptr) == VK_SUCCESS &&
      layerCount > 0) {
    std::vector<VkLayerProperties> layers(layerCount);
    if (api.vkEnumerateInstanceLayerProperties(&layerCount, layers.data()) == VK_SUCCESS) {
      for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, kValidationLayerName) == 0) {
          enabledLayers.push_back(kValidationLayerName);
          break;
        }
      }
    }
  }
  return enabledLayers;
}

/// Returns the debug-utils extension when it is enumerated and the validation layer is active,
/// otherwise an empty list. The messenger only has anything to report alongside validation.
/// @param api Resolved global entry points.
/// @param validationLayerEnabled True when the validation layer will be enabled.
std::vector<const char*> EnumerateDebugUtilsExtension(const VulkanApi& api,
                                                      bool validationLayerEnabled) {
  std::vector<const char*> enabledExtensions;
  if (!validationLayerEnabled) {
    return enabledExtensions;
  }
  uint32_t extensionCount = 0;
  if (api.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr) == VK_SUCCESS &&
      extensionCount > 0) {
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (api.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()) ==
        VK_SUCCESS) {
      for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
          enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
          break;
        }
      }
    }
  }
  return enabledExtensions;
}

/// Selects the first enumerated physical device with the target API version and a graphics queue
/// family. Returns false when none qualifies.
/// @param instance Instance to enumerate.
/// @param selectedDevice Set to the chosen physical device on success.
/// @param selectedQueueFamily Set to the chosen graphics queue family index on success.
bool SelectGraphicsPhysicalDevice(const VulkanApi& api, VkInstance instance,
                                  VkPhysicalDevice& selectedDevice, uint32_t& selectedQueueFamily) {
  uint32_t deviceCount = 0;
  if (api.vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS ||
      deviceCount == 0) {
    return false;
  }
  std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
  if (api.vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data()) !=
      VK_SUCCESS) {
    return false;
  }

  for (VkPhysicalDevice candidate : physicalDevices) {
    VkPhysicalDeviceProperties properties = {};
    api.vkGetPhysicalDeviceProperties(candidate, &properties);
    if (properties.apiVersion < kTargetApiVersion) {
      continue;
    }

    uint32_t familyCount = 0;
    api.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    api.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
    for (uint32_t familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
      if ((families[familyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
        selectedDevice = candidate;
        selectedQueueFamily = familyIndex;
        return true;
      }
    }
  }
  return false;
}

/// Whether \p physicalDevice enumerates \p extensionName.
/// @param api Resolved instance entry points. @param physicalDevice Device to query.
/// @param extensionName Extension to look for.
bool DeviceOffersExtension(const VulkanApi& api, VkPhysicalDevice physicalDevice,
                           const char* extensionName) {
  uint32_t extensionCount = 0;
  if (api.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr) !=
          VK_SUCCESS ||
      extensionCount == 0) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(extensionCount);
  if (api.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                               extensions.data()) != VK_SUCCESS) {
    return false;
  }
  return std::ranges::any_of(extensions, [extensionName](const VkExtensionProperties& extension) {
    return std::strcmp(extension.extensionName, extensionName) == 0;
  });
}

bool ContainsExtension(std::span<const char* const> extensions, const char* name) {
  return std::ranges::any_of(extensions, [name](const char* extension) {
    return extension != nullptr && std::strcmp(extension, name) == 0;
  });
}

/// Selects a device maintenance alias whose complete instance dependencies are enabled.
const char* SelectMaintenanceExtension(const VulkanApi& api,
                                       std::span<const char* const> instanceExtensions,
                                       VkPhysicalDevice physicalDevice) {
  if (!ContainsExtension(instanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
      !ContainsExtension(instanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
      !DeviceOffersExtension(api, physicalDevice, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    return nullptr;
  }
  const char* maintenanceExtension = nullptr;
  if (ContainsExtension(instanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
      DeviceOffersExtension(api, physicalDevice, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
    maintenanceExtension = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
  } else if (ContainsExtension(instanceExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
             DeviceOffersExtension(api, physicalDevice,
                                   VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
    maintenanceExtension = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
  }
  return maintenanceExtension;
}

bool HasPresentationDependencyClosure(std::span<const char* const> extensions) {
  return ContainsExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME) &&
         ContainsExtension(extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
         (ContainsExtension(extensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ||
          ContainsExtension(extensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME));
}

/// Collects the device extensions to enable and chains the feature structs they need.
///
/// Accumulates rather than assigns, so enabling a second extension or chaining a second features
/// struct cannot silently drop another one (or be dropped by it). Returns false when presentation
/// was asked for and this device cannot provide it, which is the one request here that can fail:
/// swapchain support has to be asked for before any surface exists to ask about, so a device
/// without it would otherwise refuse every surface later with nothing to point at.
///
/// @param api Resolved instance entry points.
/// @param instanceExtensions Enabled presentation instance extensions.
/// @param physicalDevice Device the extensions are checked against.
/// @param enableTimelineSemaphoreForTest Whether to request VK_KHR_timeline_semaphore.
/// @param enablePresentation Whether to request VK_KHR_swapchain.
/// @param deviceExtensions Extension names to enable; appended to.
/// @param timelineFeatures Feature struct chained when the timeline extension is requested; must
///   outlive the device creation call.
/// @param deviceInfo Creation info whose extension list and feature chain are filled in.
bool CollectDeviceExtensions(const VulkanApi& api, std::span<const char* const> instanceExtensions,
                             VkPhysicalDevice physicalDevice, bool enableTimelineSemaphoreForTest,
                             bool enablePresentation, std::vector<const char*>& deviceExtensions,
                             VkPhysicalDeviceTimelineSemaphoreFeaturesKHR& timelineFeatures,
                             VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT& maintenanceFeatures,
                             VkDeviceCreateInfo& deviceInfo) {
  timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
  timelineFeatures.timelineSemaphore = VK_TRUE;
  if (enableTimelineSemaphoreForTest) {
    deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    timelineFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
    deviceInfo.pNext = &timelineFeatures;
  }
  if (enablePresentation) {
    const char* maintenanceExtension =
        SelectMaintenanceExtension(api, instanceExtensions, physicalDevice);
    if (maintenanceExtension == nullptr) {
      return false;
    }
    deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    deviceExtensions.push_back(maintenanceExtension);
    maintenanceFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 queried = {};
    queried.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    queried.pNext = &maintenanceFeatures;
    api.vkGetPhysicalDeviceFeatures2(physicalDevice, &queried);
    if (maintenanceFeatures.swapchainMaintenance1 != VK_TRUE) {
      return false;
    }
    maintenanceFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
    deviceInfo.pNext = &maintenanceFeatures;
  }

  deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  deviceInfo.ppEnabledExtensionNames = deviceExtensions.empty() ? nullptr : deviceExtensions.data();
  return true;
}

/// Creates the logical device with the exact enabled extensions and feature chain.
VkDevice CreateLogicalDevice(const VulkanApi& api, VkPhysicalDevice selectedDevice,
                             uint32_t selectedQueueFamily,
                             std::span<const char* const> instanceExtensions,
                             bool enableTimelineSemaphoreForTest, bool enablePresentation,
                             bool& fullDrawIndexUint32) {
  // WebGPU semantics require bounds-checked buffer access; robustBufferAccess is the Vulkan
  // feature that provides it, and its support is mandatory (the specification's "Features"
  // chapter), so requiring it cannot lose devices. Fail closed anyway if a broken
  // implementation reports it unsupported.
  VkPhysicalDeviceFeatures supportedFeatures = {};
  api.vkGetPhysicalDeviceFeatures(selectedDevice, &supportedFeatures);
  if (supportedFeatures.robustBufferAccess != VK_TRUE) {
    return VK_NULL_HANDLE;
  }
  VkPhysicalDeviceFeatures enabledFeatures = {};
  enabledFeatures.robustBufferAccess = VK_TRUE;
  // Optional, so a device without it still creates; the runtime then refuses Uint32 index
  // buffers instead of letting 32-bit index values above the driver's cap read undefined data.
  fullDrawIndexUint32 = supportedFeatures.fullDrawIndexUint32 == VK_TRUE;
  enabledFeatures.fullDrawIndexUint32 = static_cast<VkBool32>(fullDrawIndexUint32);

  const float queuePriority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo = {};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = selectedQueueFamily;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &queuePriority;

  // Presentation adds swapchain maintenance with matching instance dependencies. Ordinary
  // headless devices retain the Vulkan 1.1 core feature set.
  VkDeviceCreateInfo deviceInfo = {};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.pEnabledFeatures = &enabledFeatures;

  std::vector<const char*> deviceExtensions;
  VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineFeatures = {};
  VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenanceFeatures = {};
  if (!CollectDeviceExtensions(api, instanceExtensions, selectedDevice,
                               enableTimelineSemaphoreForTest, enablePresentation, deviceExtensions,
                               timelineFeatures, maintenanceFeatures, deviceInfo)) {
    return VK_NULL_HANDLE;
  }

  VkDevice device = VK_NULL_HANDLE;
  if (api.vkCreateDevice(selectedDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }

  return device;
}

/// Resolves the device-level entry points, including the swapchain group when presentation was
/// enabled. @param loader Loader to resolve into. @param device Device to resolve against.
/// @param enablePresentation Whether the swapchain group is expected.
Status LoadDeviceEntryPoints(VulkanLoader& loader, VkDevice device, bool enablePresentation) {
  if (const Status status = loader.loadDevice(device); status.hasError()) {
    return status;
  }
  if (!enablePresentation) {
    return OkStatus();
  }
  return loader.loadPresentationDevice(device);
}

/// A debug-utils messenger plus the entry point that destroys it.
struct DebugMessengerHandles {
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;      //!< Created messenger, or null.
  PFN_vkDestroyDebugUtilsMessengerEXT destroyFn = nullptr;  //!< Destroy entry point, or null.
};

/// Best-effort: creates a debug-utils messenger that latches validation ERROR messages into the
/// device error state. On any failure the messenger stays null and validation output remains
/// log-only.
/// @param api Resolved instance entry points; the messenger functions are null when the
///   debug-utils extension was not enabled, and the messenger is then skipped.
/// @param instance Instance the messenger is created on.
/// @param userData Error state passed to the messenger callback.
DebugMessengerHandles CreateValidationMessenger(const VulkanApi& api, VkInstance instance,
                                                void* userData) {
  DebugMessengerHandles handles;
  handles.destroyFn = api.vkDestroyDebugUtilsMessengerEXT;
  if (api.vkCreateDebugUtilsMessengerEXT == nullptr || handles.destroyFn == nullptr) {
    return handles;
  }
  VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
  messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
  messengerInfo.pfnUserCallback = ValidationMessengerCallback;
  messengerInfo.pUserData = userData;
  if (api.vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &handles.messenger) !=
      VK_SUCCESS) {
    handles.messenger = VK_NULL_HANDLE;
  }
  return handles;
}

/// Sizes a descriptor pool to exactly the descriptor counts one bind group layout declares.
/// @param layoutDescriptor Layout the bind group is created against.
std::vector<VkDescriptorPoolSize> DescriptorPoolSizesFor(
    const BindGroupLayoutDescriptor& layoutDescriptor) {
  std::vector<VkDescriptorPoolSize> poolSizes;
  for (const BindGroupLayoutEntry& entry : layoutDescriptor.entries) {
    const VkDescriptorType type = ToVkDescriptorType(entry.type);
    bool found = false;
    for (VkDescriptorPoolSize& poolSize : poolSizes) {
      if (poolSize.type == type) {
        ++poolSize.descriptorCount;
        found = true;
        break;
      }
    }
    if (!found) {
      poolSizes.push_back(VkDescriptorPoolSize{type, 1});
    }
  }
  return poolSizes;
}

/// Looks up the descriptor type a layout declares for \p binding. Returns false when the layout
/// declares no such binding.
/// @param layoutDescriptor Layout the bind group is created against.
/// @param binding Binding number to look up.
/// @param outType Set to the declared descriptor type on success.
bool FindDescriptorTypeForBinding(const BindGroupLayoutDescriptor& layoutDescriptor,
                                  uint32_t binding, VkDescriptorType& outType) {
  for (const BindGroupLayoutEntry& layoutEntry : layoutDescriptor.entries) {
    if (layoutEntry.binding == binding) {
      outType = ToVkDescriptorType(layoutEntry.type);
      return true;
    }
  }
  return false;
}

/// Builds the color attachments of the compatibility render pass used for pipeline creation.
/// @param fragment Fragment stage declaring the color targets.
/// @param attachments Receives one attachment description per target.
/// @param colorRefs Receives one attachment reference per target.
void BuildCompatibilityAttachments(const FragmentState& fragment,
                                   std::vector<VkAttachmentDescription>& attachments,
                                   std::vector<VkAttachmentReference>& colorRefs) {
  for (size_t i = 0; i < fragment.targets.size(); ++i) {
    VkAttachmentDescription attachment = {};
    attachment.format = ToVkFormat(fragment.targets[i].format);
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments.push_back(attachment);
    colorRefs.push_back(
        VkAttachmentReference{static_cast<uint32_t>(i), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
  }
}

/// Translates the vertex stage's buffer layouts: vertex buffer slot N maps directly to Vulkan
/// vertex input binding N.
/// @param vertex Vertex stage declaring the buffer layouts.
/// @param vertexBindings Receives one binding description per buffer layout.
/// @param vertexAttributes Receives one attribute description per attribute.
void BuildVertexInputDescriptions(
    const VertexState& vertex, std::vector<VkVertexInputBindingDescription>& vertexBindings,
    std::vector<VkVertexInputAttributeDescription>& vertexAttributes) {
  for (size_t bufferIndex = 0; bufferIndex < vertex.buffers.size(); ++bufferIndex) {
    const VertexBufferLayout& layoutDescriptor = vertex.buffers[bufferIndex];
    VkVertexInputBindingDescription binding = {};
    binding.binding = static_cast<uint32_t>(bufferIndex);
    binding.stride = layoutDescriptor.strideBytes;
    binding.inputRate = ToVkInputRate(layoutDescriptor.stepMode);
    vertexBindings.push_back(binding);
    for (const VertexAttribute& attribute : layoutDescriptor.attributes) {
      VkVertexInputAttributeDescription attributeDescription = {};
      attributeDescription.location = attribute.shaderLocation;
      attributeDescription.binding = static_cast<uint32_t>(bufferIndex);
      attributeDescription.format = ToVkVertexFormat(attribute.format);
      attributeDescription.offset = attribute.offsetBytes;
      vertexAttributes.push_back(attributeDescription);
    }
  }
}

/// Translates each color target's blend state into a Vulkan blend attachment.
/// @param fragment Fragment stage declaring the color targets.
std::vector<VkPipelineColorBlendAttachmentState> BuildBlendAttachments(
    const FragmentState& fragment) {
  std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
  for (const ColorTargetState& target : fragment.targets) {
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    if (target.blend.has_value()) {
      blendAttachment.blendEnable = VK_TRUE;
      blendAttachment.srcColorBlendFactor = ToVkBlendFactor(target.blend->color.srcFactor);
      blendAttachment.dstColorBlendFactor = ToVkBlendFactor(target.blend->color.dstFactor);
      blendAttachment.colorBlendOp = ToVkBlendOp(target.blend->color.operation);
      blendAttachment.srcAlphaBlendFactor = ToVkBlendFactor(target.blend->alpha.srcFactor);
      blendAttachment.dstAlphaBlendFactor = ToVkBlendFactor(target.blend->alpha.dstFactor);
      blendAttachment.alphaBlendOp = ToVkBlendOp(target.blend->alpha.operation);
    } else {
      blendAttachment.blendEnable = VK_FALSE;
    }
    blendAttachment.colorWriteMask = ToVkColorWriteMask(target.writeMask);
    blendAttachments.push_back(blendAttachment);
  }
  return blendAttachments;
}

/// Destroys \p handle through \p destroyFn when it is set, then clears it.
/// @param device Owning logical device.
/// @param handle Vulkan handle to destroy.
/// @param destroyFn Vulkan destroy entry point for the handle type.
template <typename Handle, typename DestroyFn>
void DestroyIfSet(VkDevice device, Handle& handle, DestroyFn destroyFn) {
  if (handle != VK_NULL_HANDLE) {
    destroyFn(device, handle, nullptr);
    handle = VK_NULL_HANDLE;
  }
}

/// The Vulkan instance a device is built on, together with the loader that resolved it.
///
/// An empty setup (null loader) means instance creation failed; the reason has already been
/// reported.
struct InstanceSetup {
  std::shared_ptr<VulkanLoader> loader;  //!< Loader kept open for the instance's lifetime.
  VkInstance instance = VK_NULL_HANDLE;  //!< Created instance, or null on failure.
  bool debugMessengerAvailable = false;  //!< True when debug-utils was enabled on it.
  std::vector<const char*> presentationExtensions;  //!< Enabled surface extension closure.
  bool headlessSurfaceAvailable = false;  //!< True when headless surfaces were enabled with them.
};

/// Returns the surface extensions presentation needs, or an empty list when the loader does not
/// offer the complete maintenance1 dependency closure. VK_EXT_headless_surface is optional and lets
/// the presentation contract be exercised on a machine with no display at all.
/// @param api Resolved global entry points.
std::vector<const char*> EnumeratePresentationExtensions(
    const VulkanApi& api, std::span<const char* const> requiredExtensions) {
  if (requiredExtensions.size() > std::numeric_limits<uint32_t>::max()) {
    return {};
  }
  std::vector<const char*> enabledExtensions;
  uint32_t extensionCount = 0;
  if (api.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr) != VK_SUCCESS ||
      extensionCount == 0) {
    return enabledExtensions;
  }
  std::vector<VkExtensionProperties> extensions(extensionCount);
  if (api.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()) !=
      VK_SUCCESS) {
    return enabledExtensions;
  }

  std::vector<const char*> offeredExtensions;
  offeredExtensions.reserve(extensions.size());
  for (const VkExtensionProperties& extension : extensions) {
    offeredExtensions.push_back(extension.extensionName);
  }
  return SelectPresentationExtensionsForTest(offeredExtensions, requiredExtensions);
}

/// Creates an instance with the complete requested presentation dependency closure.
VkInstance CreateNativeInstance(const VulkanApi& api, bool withPresentation,
                                std::span<const char* const> requiredExtensions,
                                bool& debugUtilsEnabled,
                                std::vector<const char*>& presentationExtensions) {
  // Vulkan 1.1 requires instance-level 1.1 support. On a 1.0-only loader
  // vkEnumerateInstanceVersion still exists as a loader export in loaders new enough for this
  // backend's deployment targets; a version below 1.1 fails closed here.
  uint32_t instanceVersion = 0;
  if (api.vkEnumerateInstanceVersion(&instanceVersion) != VK_SUCCESS ||
      instanceVersion < kTargetApiVersion) {
    return VK_NULL_HANDLE;
  }

  // Enable the Khronos validation layer only when the loader enumerates it, and the debug-utils
  // messenger extension only alongside it: the messenger turns validation ERROR messages into
  // latched device errors (see ValidationMessengerCallback) instead of log lines.
  const std::vector<const char*> enabledLayers = EnumerateValidationLayer(api);
  std::vector<const char*> enabledExtensions =
      EnumerateDebugUtilsExtension(api, !enabledLayers.empty());
  debugUtilsEnabled = !enabledExtensions.empty();

  // Presentation is opt in, so the default instance stays exactly as headless as it was.
  if (withPresentation) {
    presentationExtensions = EnumeratePresentationExtensions(api, requiredExtensions);
    if (presentationExtensions.empty()) {
      return VK_NULL_HANDLE;
    }
    enabledExtensions.insert(enabledExtensions.end(), presentationExtensions.begin(),
                             presentationExtensions.end());
  }

  VkApplicationInfo applicationInfo = {};
  applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  applicationInfo.pApplicationName = "donner";
  applicationInfo.applicationVersion = 1;
  applicationInfo.pEngineName = "donner-gpu";
  applicationInfo.engineVersion = 1;
  applicationInfo.apiVersion = kTargetApiVersion;

  VkInstanceCreateInfo instanceInfo = {};
  instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceInfo.pApplicationInfo = &applicationInfo;
  instanceInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
  instanceInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
  // Headless: no surface extensions; debug utils only (see above).
  instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
  instanceInfo.ppEnabledExtensionNames =
      enabledExtensions.empty() ? nullptr : enabledExtensions.data();

  VkInstance instance = VK_NULL_HANDLE;
  if (api.vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }

  return instance;
}

/// Opens the Vulkan loader, checks that it offers the API version this backend targets, and
/// creates the instance with the validation layer and debug-utils messenger extension when they
/// are available, plus the surface extensions when presentation was asked for.
/// @param withPresentation Whether to enable the surface extensions as well.
InstanceSetup CreateInstance(bool withPresentation = false,
                             std::span<const char* const> requiredExtensions = {}) {
  InstanceSetup setup;
  // Vulkan is reached entirely through the loader this opens: nothing in this backend is a
  // link-time symbol, so a machine without a Vulkan runtime reports it here instead of failing
  // to start.
  Result<std::shared_ptr<VulkanLoader>> loaderResult = VulkanLoader::Open();
  if (loaderResult.hasError()) {
    std::fprintf(stderr, "[donner::gpu::vulkan] %s\n", loaderResult.error().message.c_str());
    return {};
  }
  const std::shared_ptr<VulkanLoader> loader = std::move(loaderResult).result();
  const VulkanApi& api = loader->api();

  bool debugUtilsEnabled = false;
  std::vector<const char*> presentationExtensions;
  const VkInstance instance = CreateNativeInstance(api, withPresentation, requiredExtensions,
                                                   debugUtilsEnabled, presentationExtensions);
  if (instance == VK_NULL_HANDLE) {
    return {};
  }

  // Instance-level entry points exist only once an instance does. Nothing below may be called
  // before this succeeds; on failure the instance is unreachable except through the one entry
  // point that destroys it, so release it only when that one resolved.
  if (const Status status = loader->loadInstance(instance, debugUtilsEnabled); status.hasError()) {
    std::fprintf(stderr, "[donner::gpu::vulkan] %s\n", status.error().message.c_str());
    if (api.vkDestroyInstance != nullptr) {
      api.vkDestroyInstance(instance, nullptr);
    }
    return {};
  }

  if (!presentationExtensions.empty()) {
    const bool headlessSurfaceEnabled =
        std::ranges::any_of(presentationExtensions, [](const char* name) {
          return std::strcmp(name, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0;
        });
    if (const Status status = loader->loadPresentationInstance(instance, headlessSurfaceEnabled);
        status.hasError()) {
      std::fprintf(stderr, "[donner::gpu::vulkan] %s\n", status.error().message.c_str());
      api.vkDestroyInstance(instance, nullptr);
      return {};
    }
    setup.headlessSurfaceAvailable = headlessSurfaceEnabled;
    setup.presentationExtensions = std::move(presentationExtensions);
  }

  setup.loader = loader;
  setup.instance = instance;
  setup.debugMessengerAvailable = debugUtilsEnabled;
  return setup;
}

}  // namespace

std::vector<const char*> SelectPresentationExtensionsForTest(
    std::span<const char* const> offeredExtensions,
    std::span<const char* const> requiredExtensions) {
  const auto offers = [offeredExtensions](const char* name) {
    return ContainsExtension(offeredExtensions, name);
  };
  if (!HasPresentationDependencyClosure(offeredExtensions)) {
    return {};
  }
  std::vector<const char*> enabledExtensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
  for (const char* name :
       {VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME}) {
    if (offers(name)) {
      enabledExtensions.push_back(name);
    }
  }
  if (offers(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
    enabledExtensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
  }
  for (const char* required : requiredExtensions) {
    if (required == nullptr || !offers(required)) {
      return {};
    }
    if (std::ranges::none_of(enabledExtensions, [required](const char* enabled) {
          return std::strcmp(enabled, required) == 0;
        })) {
      enabledExtensions.push_back(required);
    }
  }
  return enabledExtensions;
}

/// Vulkan state of a VulkanDevice: instance/device/queue handles plus per-resource slot tables
/// mirroring the validated slot indices handed to the `on*` hooks.
struct VulkanDevice::Impl {
  /// Lives until process exit so neither the gate nor a quarantined loader is destructed.
  struct AdmissionGate {
    std::mutex mutex;
    bool closed = false;
    Impl* retained = nullptr;
  };

  static AdmissionGate& admissionGate() {
    static AdmissionGate* const gate = new AdmissionGate;
    return *gate;
  }

  Impl* retainedNext = nullptr;      //!< Intrusive quarantine link; insertion cannot allocate.
  bool executionUncertain = false;   //!< Failed submission may still reference every resource.
  std::optional<VulkanApi> testApi;  //!< Owns fake entry points when no loader exists.
  std::shared_ptr<VulkanSurfaceLifetime> surfaceLifetime =
      std::make_shared<VulkanSurfaceLifetime>();  //!< Preallocated child lifetime signal.

  /// Preserves the complete native ownership graph while the caller holds the admission lock.
  static void quarantineUnderAdmissionLock(std::unique_ptr<Impl> impl, AdmissionGate& gate) {
    gate.closed = true;
    impl->retainedNext = gate.retained;
    gate.retained = impl.release();
  }

  /// Keeps the Vulkan loader library open. Every entry point this device calls is code inside
  /// that library, so it must outlive every Vulkan object below.
  std::shared_ptr<VulkanLoader> loader;
  /// Entry points resolved for this instance and device; owned by \ref loader.
  const VulkanApi* api = nullptr;

  VkInstance instance = VK_NULL_HANDLE;                    //!< Owning instance; set by Create.
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;        //!< Selected physical device.
  VkPhysicalDeviceMemoryProperties memoryProperties = {};  //!< Memory heaps/types of the device.
  /// Where buffer memory comes from. One dedicated allocation per buffer today; the seam exists
  /// so a suballocating implementation can replace that without touching any call site.
  std::unique_ptr<BufferSuballocator> bufferAllocator;
  VkDevice device = VK_NULL_HANDLE;            //!< Logical device.
  VkQueue queue = VK_NULL_HANDLE;              //!< The single graphics queue.
  uint32_t queueFamilyIndex = 0;               //!< Family index of \ref queue.
  VkCommandPool commandPool = VK_NULL_HANDLE;  //!< Pool for all command buffers.
  bool fullDrawIndexUint32 = false;            //!< Whether the full Uint32 index range is enabled.

  /// A buffer plus the memory it was bound into, persistently mapped (host-visible + coherent;
  /// see the class comment for why every buffer is host-visible in this slice). Where that
  /// memory comes from is the allocator's decision, not this record's.
  struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;  //!< Buffer handle.
    BufferAllocation allocation;       //!< Memory this buffer was bound into.
    VkDeviceSize byteSize = 0;         //!< Creation size in bytes.
    uint64_t uploadSerial = 0;  //!< Last submission containing a queued write to this buffer.
  };

  /// An image plus its dedicated allocation. Its synchronization state lives in \ref syncStates
  /// rather than here, because that state is staged during an encode and committed only once the
  /// submission reached the queue. Tracking at encode time is valid because submissions execute
  /// in order on the single queue.
  struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;                    //!< Image handle.
    VkDeviceMemory memory = VK_NULL_HANDLE;            //!< Dedicated allocation.
    TextureFormat format = TextureFormat::RGBA8Unorm;  //!< RHI format (for copy texel math).
    Extent2d size;                                     //!< Extent in texels.
    TextureUsage usage = TextureUsage::None;           //!< RHI usage flags.
    /// False for a frame acquired from a surface: the swapchain owns those images and destroys
    /// them with itself, so destroying one here would destroy an image this device never made.
    bool ownsImage = true;
  };

  /// A view of a texture slot; the VkImageView is created once at view creation.
  struct TextureViewRecord {
    VkImageView view = VK_NULL_HANDLE;  //!< View handle.
    uint32_t textureSlot = 0;           //!< Slot of the viewed texture.
  };

  /// A descriptor set layout plus the validated descriptor it was created from (snapshotted so
  /// bind-group creation never depends on later slot state).
  struct BindGroupLayoutRecord {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;  //!< Set layout handle.
    BindGroupLayoutDescriptor descriptor;           //!< Validated creation descriptor.
  };

  /// A descriptor set plus its dedicated one-set pool (destroying the pool frees the set) and
  /// the texture slots its sampled-texture entries reference (snapshotted at creation), so
  /// encode can pre-transition sampled textures to the layout the descriptor writes declare.
  struct BindGroupRecord {
    VkDescriptorPool pool = VK_NULL_HANDLE;     //!< Dedicated descriptor pool.
    VkDescriptorSet set = VK_NULL_HANDLE;       //!< The allocated descriptor set.
    std::vector<uint32_t> sampledTextureSlots;  //!< Texture slots of sampled-texture entries.
    std::vector<uint32_t> storageTextureSlots;  //!< Texture slots of storage-texture entries.
  };

  /// Shared ownership wrapper for a VkPipelineLayout. Pipelines retain the wrapper so
  /// vkCmdBindDescriptorSets at encode time stays valid even after the RHI PipelineLayout
  /// resource (exempt from submission pinning) is destroyed - the "snapshot or retain"
  /// requirement in Device.h.
  struct PipelineLayoutHandle {
    /// Entry points used to destroy \ref layout. Held here because this outlives the slot it was
    /// created for: a pipeline retains its layout, so the destructor can run after the owning
    /// slot is gone.
    const VulkanApi* api = nullptr;
    VkDevice device = VK_NULL_HANDLE;          //!< Device that owns \ref layout.
    VkPipelineLayout layout = VK_NULL_HANDLE;  //!< Pipeline layout handle.
    uint32_t descriptorSetCount = 0;           //!< Number of descriptor sets in the layout.

    /// Destructor; destroys the pipeline layout.
    ~PipelineLayoutHandle() {
      if (layout != VK_NULL_HANDLE && api != nullptr) {
        api->vkDestroyPipelineLayout(device, layout, nullptr);
      }
    }
  };

  /// A compiled graphics pipeline, its compatibility render pass (used only for pipeline
  /// creation; per-submission passes are compatible by format/sample-count), and the retained
  /// pipeline layout.
  struct RenderPipelineRecord {
    VkPipeline pipeline = VK_NULL_HANDLE;            //!< Compiled pipeline.
    VkRenderPass compatRenderPass = VK_NULL_HANDLE;  //!< Pipeline-creation render pass.
    std::shared_ptr<PipelineLayoutHandle> layout;    //!< Retained pipeline layout.
  };

  std::vector<std::optional<BufferRecord>> buffers;                    //!< Buffer slots.
  std::vector<std::optional<TextureRecord>> textures;                  //!< Texture slots.
  std::vector<std::optional<TextureViewRecord>> textureViews;          //!< View slots.
  std::vector<VkSampler> samplers;                                     //!< Sampler slots.
  std::vector<std::optional<BindGroupLayoutRecord>> bindGroupLayouts;  //!< Layout slots.
  std::vector<std::optional<BindGroupRecord>> bindGroups;              //!< Bind group slots.
  std::vector<std::shared_ptr<PipelineLayoutHandle>> pipelineLayouts;  //!< Pipeline layouts.
  std::vector<VkShaderModule> shaderModules;                           //!< Shader module slots.
  /// A compiled compute pipeline plus the retained pipeline layout its descriptor binds need.
  struct ComputePipelineRecord {
    VkPipeline pipeline = VK_NULL_HANDLE;          //!< Compiled pipeline.
    std::shared_ptr<PipelineLayoutHandle> layout;  //!< Retained pipeline layout.
  };

  std::vector<std::optional<RenderPipelineRecord>> renderPipelines;    //!< Pipeline slots.
  std::vector<std::optional<ComputePipelineRecord>> computePipelines;  //!< Compute pipeline slots.

  /// One submitted command buffer awaiting fence completion, with the transient render passes
  /// and framebuffers its encoding created.
  struct InFlightSubmission {
    uint64_t serial = 0;                          //!< Submission serial.
    VkFence fence = VK_NULL_HANDLE;               //!< Signaled when the submission completes.
    std::vector<VkCommandBuffer> commandBuffers;  //!< Submitted buffers, in execution order.
    std::vector<VkRenderPass> renderPasses;       //!< Transient per-pass render passes.
    std::vector<VkFramebuffer> framebuffers;      //!< Transient per-pass framebuffers.
    BufferRecord bufferWriteStaging;              //!< Packed queued-write payload.
    std::vector<BufferRecord> retiredBuffers;     //!< Upload destinations whose slots were freed.
  };

  /// A copied host payload awaiting an ordinary submission, ordered by write call.
  struct PendingBufferWrite {
    uint32_t slotIndex = 0;      //!< Destination slot; discarded when the buffer is retired.
    uint64_t offsetBytes = 0;    //!< Destination byte offset.
    std::vector<uint8_t> bytes;  //!< Owned payload, independent of caller storage.
  };

  std::vector<PendingBufferWrite> pendingBufferWrites;  //!< Unsent writes in call order.
  uint64_t pendingBufferWriteBytes = 0;                 //!< Bytes reserved by unsent writes.
  uint64_t inFlightBufferWriteBytes = 0;                //!< Staging bytes retained by submissions.
  uint64_t bufferWriteByteBudget = kMaxBufferByteSize;  //!< Combined pending/in-flight limit.
  uint64_t bufferWriteStagingAllocations = 0;           //!< Successful packed staging allocations.
  uint64_t submittedBufferWriteBatches = 0;             //!< Submitted batches with queued writes.
  VkResult nextSubmissionFailure = VK_SUCCESS;  //!< Test-only failure before queue submission.
  uint64_t lostDeviceDrains = 0;  //!< Terminal submission failures drained before cleanup.

  /// Whether this destination has older unsent writes that a host copy must not overtake.
  bool hasPendingBufferWrite(uint32_t slotIndex) const {
    return std::ranges::any_of(pendingBufferWrites, [slotIndex](const PendingBufferWrite& write) {
      return write.slotIndex == slotIndex;
    });
  }

  /// Discards writes to a retired buffer, or writes just applied through its idle mapping.
  void discardPendingBufferWrites(uint32_t slotIndex) {
    std::erase_if(pendingBufferWrites, [&](const PendingBufferWrite& write) {
      if (write.slotIndex != slotIndex) {
        return false;
      }
      pendingBufferWriteBytes -= write.bytes.size();
      return true;
    });
  }

  /// Copies a write into the bounded queue; identical ranges coalesce at the newest position.
  Status queueBufferWrite(uint32_t slotIndex, uint64_t offsetBytes,
                          std::span<const uint8_t> bytes) {
    const auto previous =
        std::ranges::find_if(pendingBufferWrites, [&](const PendingBufferWrite& write) {
          return write.slotIndex == slotIndex && write.offsetBytes == offsetBytes &&
                 write.bytes.size() == bytes.size();
        });
    const uint64_t replacedBytes =
        previous == pendingBufferWrites.end() ? 0 : previous->bytes.size();
    const uint64_t queuedBytes = pendingBufferWriteBytes - replacedBytes + bytes.size();
    if (queuedBytes > bufferWriteByteBudget ||
        inFlightBufferWriteBytes > bufferWriteByteBudget - queuedBytes ||
        (previous == pendingBufferWrites.end() && pendingBufferWrites.size() >= 16'384)) {
      return GpuError{GpuErrorType::LimitExceeded,
                      std::format("Vulkan buffer write budget exceeded: {} queued / {} in-flight "
                                  "bytes (limit {}), {} pending writes",
                                  queuedBytes, inFlightBufferWriteBytes, bufferWriteByteBudget,
                                  pendingBufferWrites.size())};
    }
    PendingBufferWrite write{.slotIndex = slotIndex, .offsetBytes = offsetBytes};
    if (previous != pendingBufferWrites.end()) {
      write.bytes = std::move(previous->bytes);
      pendingBufferWrites.erase(previous);
    }
    write.bytes.assign(bytes.begin(), bytes.end());
    pendingBufferWrites.push_back(std::move(write));
    pendingBufferWriteBytes = queuedBytes;
    return OkStatus();
  }

  /// Applies older queued writes before an unaligned host write to an idle destination.
  void flushPendingBufferWritesToHost(uint32_t slotIndex, void* mapped) {
    for (const PendingBufferWrite& write : pendingBufferWrites) {
      if (write.slotIndex == slotIndex) {
        std::memcpy(static_cast<uint8_t*>(mapped) + write.offsetBytes, write.bytes.data(),
                    write.bytes.size());
      }
    }
    discardPendingBufferWrites(slotIndex);
  }

  /// Records accepted writes' last-use serials and releases their pending payloads.
  void commitPendingBufferWrites(uint64_t submissionSerial) {
    if (pendingBufferWrites.empty()) {
      return;
    }
    for (const PendingBufferWrite& write : pendingBufferWrites) {
      FindRecord(buffers, write.slotIndex)->uploadSerial = submissionSerial;
    }
    inFlightBufferWriteBytes += pendingBufferWriteBytes;
    pendingBufferWrites.clear();
    pendingBufferWriteBytes = 0;
    ++submittedBufferWriteBatches;
  }

  /// Upload objects retained when the queue accepted the copy but its wait timed out.
  struct PendingUpload {
    VkFence fence = VK_NULL_HANDLE;                  //!< Completion fence.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;  //!< Copy commands.
    BufferRecord staging;                            //!< Host-visible upload memory.
    uint32_t textureSlot = 0;  //!< Destination slot, until its texture is retired.
    std::optional<TextureRecord> retiredTexture;  //!< Destination released by the caller.
  };

  std::vector<PendingUpload> pendingUploads;  //!< Uploads awaiting fence-confirmed cleanup.

  bool deferUploadPolling = false;  //!< Test-only deferral of upload completion observations.
  /// Test-only observer of each timed-out step of a serial wait's fence wait.
  std::function<void()> fenceWaitStepHookForTest;

  /// Releases one completed upload and its optional retired destination.
  void releaseUpload(PendingUpload& upload) {
    destroyUploadObjects(upload.fence, upload.commandBuffer, upload.staging);
    if (upload.retiredTexture) {
      destroyTextureRecord(*upload.retiredTexture);
    }
  }

  /// Reclaims completed upload objects without waiting or advancing public submission serials.
  void pollUploads() {
    if (deferUploadPolling) {
      return;
    }
    auto it = pendingUploads.begin();
    while (it != pendingUploads.end()) {
      const VkResult status = api->vkGetFenceStatus(device, it->fence);
      if (!CompletionWasProven(status)) {
        if (status != VK_NOT_READY) {
          recordError(
              std::format("vkGetFenceStatus (upload) failed with {}", VkResultToString(status)));
        }
        break;
      }
      if (status == VK_ERROR_DEVICE_LOST) {
        recordDeviceLoss("upload fence reported device loss");
      }
      releaseUpload(*it);
      it = pendingUploads.erase(it);
    }
  }

  std::vector<InFlightSubmission> inFlight;  //!< Pending submissions, ascending serial.
  uint64_t completedSerialValue = 0;         //!< Highest fence-confirmed completed serial.

  /// Latched first failure (fence wait/poll errors, validation ERROR messages). Held by
  /// unique_ptr so the messenger callback's userData pointer stays stable.
  std::unique_ptr<ErrorState> errorState = std::make_unique<ErrorState>();

  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;  //!< Validation message latch.
  /// vkDestroyDebugUtilsMessengerEXT, resolved at instance creation (extension functions are
  /// not exported by the loader statically).
  PFN_vkDestroyDebugUtilsMessengerEXT destroyDebugMessengerFn = nullptr;

  /// Records the first failure (fence wait/poll error or validation error).
  void recordError(std::string message) { errorState->record(std::move(message)); }

  /// Loss condition of the root this device opens over, which the base device reports through
  /// `isLost()`; shared with every other device over that root.
  std::shared_ptr<DeviceLostState> rootLoss = std::make_shared<DeviceLostState>();

  /// Records a device loss the driver reported. The root is declared lost first, as a
  /// backend-reported loss with no wait site, and the error only then, so a waiter that sees the
  /// error also sees the loss and cannot give up first and record a timeout of its own.
  /// @param message Diagnostic for the device error and for the loss log line.
  void recordDeviceLoss(const std::string& message) {
    const bool declared = DeclareDeviceLost(*rootLoss);
    recordError(message);
    if (declared) {
      LogDeclaredDeviceLoss(message.c_str());
    }
  }

  /// Whether a child failed to prove its native work complete.
  bool surfaceLifetimeUnproven() const {
    return surfaceLifetime->unproven.load(std::memory_order_acquire);
  }

  bool hasError() const {
    return surfaceLifetimeUnproven() || errorState->hadError.load(std::memory_order_acquire);
  }

  std::string errorMessage() const {
    return surfaceLifetimeUnproven() ? "Vulkan surface destruction did not prove completion"
                                     : errorState->firstMessage();
  }

  /// The Vulkan answers behind a host mapping. Nested here because it reads the buffer slot
  /// table, whose record type is private to this implementation; the bodies are defined with the
  /// mapping hooks at the end of this file.
  class MappingHost final : public BufferMappingHost {
  public:
    /// @param device Device whose submissions decide readiness. @param impl State of \p device.
    MappingHost(VulkanDevice& device, Impl& impl) : device_(device), impl_(impl) {}

    std::span<const uint8_t> mappableBytes(uint32_t bufferSlotIndex) const override;
    uint64_t completedSubmissionSerial() const override;
    MapWaitKind waitForSubmission(uint64_t serial, double sliceSeconds) override;
    bool deviceLost() const override;

  private:
    VulkanDevice& device_;  //!< Device whose submissions decide readiness.
    Impl& impl_;            //!< Buffer slots and error state.
  };

  std::optional<MappingHost> mappingHost;          //!< Created with the first mapping.
  std::optional<BufferMappingTable> mappingTable;  //!< Open host mappings.

  /// Submits command buffers as one queue submission, with the shared one-shot native failure
  /// seam for tests.
  /// @param commandBuffers Command buffers to submit, in execution order.
  /// @param fence Fence signaled once every buffer of the submission has finished.
  /// @param wait Semaphores this submission must wait on, empty for internal work; a frame
  ///   acquired from a surface comes back before the presentation engine has finished reading
  ///   it, so the submission that writes it waits here.
  VkResult submitToQueue(std::span<const VkCommandBuffer> commandBuffers, VkFence fence,
                         const SurfaceWaitSync& wait = {}) {
    // One queue submission for the whole span: the buffers execute in the order given, and the
    // fence signals once, when all of them have finished.
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
    submitInfo.pCommandBuffers = commandBuffers.data();
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(wait.semaphores.size());
    submitInfo.pWaitSemaphores = wait.semaphores.empty() ? nullptr : wait.semaphores.data();
    submitInfo.pWaitDstStageMask = wait.stages.empty() ? nullptr : wait.stages.data();
    const VkResult injectedFailure = std::exchange(nextSubmissionFailure, VK_SUCCESS);
    return injectedFailure != VK_SUCCESS ? injectedFailure
                                         : api->vkQueueSubmit(queue, 1, &submitInfo, fence);
  }

  /// Latches terminal native failure and drains pending work before releasing its objects.
  /// @param result Native operation result, before any transient resources are released.
  /// @param operation Operation name included in the latched diagnostic.
  bool drainAfterDeviceLoss(VkResult result, std::string_view operation) {
    if (result != VK_ERROR_DEVICE_LOST) {
      return false;
    }
    recordDeviceLoss(std::format("{} failed with {}", operation, VkResultToString(result)));
    // Only device loss permits this otherwise unbounded wait: the lost-device wait is finite.
    if (!CompletionWasProven(api->vkDeviceWaitIdle(device))) {
      return false;
    }
    ++lostDeviceDrains;
    return true;
  }

  /// Destroys the debug-utils messenger; must run before the instance is destroyed.
  void destroyDebugMessenger() {
    if (debugMessenger != VK_NULL_HANDLE && destroyDebugMessengerFn != nullptr &&
        instance != VK_NULL_HANDLE) {
      destroyDebugMessengerFn(instance, debugMessenger, nullptr);
      debugMessenger = VK_NULL_HANDLE;
    }
  }

  /// Destroys the transient objects and command buffer of a completed submission.
  void releaseSubmission(InFlightSubmission& submission) {
    inFlightBufferWriteBytes -= submission.bufferWriteStaging.byteSize;
    destroyBufferRecord(submission.bufferWriteStaging);
    for (BufferRecord& record : submission.retiredBuffers) {
      destroyBufferRecord(record);
    }
    for (VkFramebuffer framebuffer : submission.framebuffers) {
      api->vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    for (VkRenderPass renderPass : submission.renderPasses) {
      api->vkDestroyRenderPass(device, renderPass, nullptr);
    }
    if (!submission.commandBuffers.empty()) {
      api->vkFreeCommandBuffers(device, commandPool,
                                static_cast<uint32_t>(submission.commandBuffers.size()),
                                submission.commandBuffers.data());
    }
    if (submission.fence != VK_NULL_HANDLE) {
      api->vkDestroyFence(device, submission.fence, nullptr);
    }
  }

  /// Polls pending fences in submission order, releasing completed submissions and advancing
  /// the monotonic completed-serial counter. Stops at the first unsignaled fence (fences on one
  /// queue signal in submission order).
  void pollCompleted() {
    if (executionUncertain || surfaceLifetimeUnproven()) {
      return;
    }
    pollUploads();
    size_t releasedCount = 0;
    for (InFlightSubmission& submission : inFlight) {
      const VkResult status = api->vkGetFenceStatus(device, submission.fence);
      if (CompletionWasProven(status)) {
        if (status == VK_ERROR_DEVICE_LOST) {
          recordDeviceLoss("submission fence reported device loss");
        }
        releaseSubmission(submission);
        completedSerialValue = submission.serial;
        ++releasedCount;
      } else {
        if (status != VK_NOT_READY) {
          recordError(std::format("vkGetFenceStatus failed with {}", VkResultToString(status)));
        }
        break;
      }
    }
    if (releasedCount > 0) {
      inFlight.erase(inFlight.begin(), inFlight.begin() + static_cast<ptrdiff_t>(releasedCount));
    }
  }

  /// The earliest in-flight submission whose completion covers \p serial, or null when no
  /// submission through that serial is in flight. @param serial Serial to cover.
  const InFlightSubmission* firstInFlightThrough(uint64_t serial) const {
    const auto found = std::ranges::find_if(
        inFlight,
        [serial](const InFlightSubmission& submission) { return submission.serial >= serial; });
    return found == inFlight.end() ? nullptr : &*found;
  }

  /// How a stepped fence wait ended.
  enum class FenceWaitEnd : uint8_t {
    Signalled,  //!< The fence signalled.
    TimedOut,   //!< The budget ran out first.
    RootLost,   //!< A device over the root declared the root lost first.
    Failed,     //!< The wait itself failed; \ref FenceWait::result says how.
  };

  /// How a stepped fence wait ended, with the native result of its last step.
  struct FenceWait {
    FenceWaitEnd end = FenceWaitEnd::TimedOut;  //!< How the wait ended.
    VkResult result = VK_TIMEOUT;               //!< Native result of the last step.
  };

  /// Waits for \p fence for up to \p timeoutSeconds, in steps of at most 10 ms so that a loss
  /// another device over the root declares while the wait is blocked ends it too. Records
  /// nothing; each caller decides what its outcome means.
  /// @param fence Fence to wait for.
  /// @param timeoutSeconds Budget in seconds, already bounded by the caller.
  FenceWait waitForFenceUnlessLost(VkFence fence, double timeoutSeconds) {
    constexpr std::chrono::nanoseconds kLossCheckInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::duration<double>(std::max(timeoutSeconds, 0.0)));
    while (true) {
      const std::chrono::nanoseconds remaining =
          std::max(std::chrono::nanoseconds::zero(), deadline - std::chrono::steady_clock::now());
      const std::chrono::nanoseconds step = std::min(remaining, kLossCheckInterval);
      const VkResult result =
          api->vkWaitForFences(device, 1, &fence, VK_TRUE, static_cast<uint64_t>(step.count()));
      if (result == VK_SUCCESS) {
        return {FenceWaitEnd::Signalled, result};
      }
      if (result != VK_TIMEOUT) {
        return {FenceWaitEnd::Failed, result};
      }
      if (fenceWaitStepHookForTest) {
        fenceWaitStepHookForTest();
      }
      if (rootLoss->lost.load(std::memory_order_acquire)) {
        return {FenceWaitEnd::RootLost, result};
      }
      if (remaining <= step) {
        return {FenceWaitEnd::TimedOut, result};
      }
    }
  }

  /// Records a fence wait that failed with \p result. @param result Native result of the wait.
  void recordFenceWaitFailure(VkResult result) {
    std::string message = std::format("vkWaitForFences failed with {}", VkResultToString(result));
    if (result == VK_ERROR_DEVICE_LOST) {
      recordDeviceLoss(message);
    } else {
      recordError(std::move(message));
    }
  }

  /// Allocates one primary command buffer from the pool.
  Result<VkCommandBuffer> allocateCommandBuffer() {
    if (hasError()) {
      return GpuError{GpuErrorType::InvalidState, errorMessage()};
    }
    VkCommandBufferAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (const VkResult result =
            api->vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer);
        result != VK_SUCCESS) {
      return VkError("vkAllocateCommandBuffers", result);
    }
    return commandBuffer;
  }

  /// Creates a buffer and binds it to memory from the allocator. Used for both RHI buffers and
  /// internal upload staging, so both benefit from whatever the allocator does.
  Result<BufferRecord> createHostVisibleBuffer(VkDeviceSize byteSize, VkBufferUsageFlags usage,
                                               std::string_view label) {
    BufferRecord record;
    record.byteSize = byteSize;

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteSize;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (const VkResult result = api->vkCreateBuffer(device, &bufferInfo, nullptr, &record.buffer);
        result != VK_SUCCESS) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("vkCreateBuffer for '{}' failed with {}", label, VkResultToString(result))};
    }

    Result<BufferAllocation> allocation =
        bufferAllocator->allocate(*api, device, record.buffer, label);
    if (allocation.hasError()) {
      destroyBufferRecord(record);
      return std::move(allocation).error();
    }
    record.allocation = std::move(allocation).result();
    return record;
  }

  /// Destroys a buffer record's Vulkan objects (mapping is released implicitly by the free).
  void destroyBufferRecord(BufferRecord& record) {
    if (record.buffer != VK_NULL_HANDLE) {
      api->vkDestroyBuffer(device, record.buffer, nullptr);
      record.buffer = VK_NULL_HANDLE;
    }
    if (bufferAllocator != nullptr) {
      bufferAllocator->release(*api, device, record.allocation);
    }
  }

  /// Destroys a texture record's Vulkan objects.
  void destroyTextureRecord(TextureRecord& record) {
    if (!record.ownsImage) {
      record.image = VK_NULL_HANDLE;
      return;
    }
    if (record.image != VK_NULL_HANDLE) {
      api->vkDestroyImage(device, record.image, nullptr);
      record.image = VK_NULL_HANDLE;
    }
    if (record.memory != VK_NULL_HANDLE) {
      api->vkFreeMemory(device, record.memory, nullptr);
      record.memory = VK_NULL_HANDLE;
    }
  }

  /// Refuses teardown while any child is untracked or has failed its own destruction proof.
  bool ownsEverySurface() const {
    if (surfaceLifetimeUnproven()) {
      return false;
    }
    size_t owned =
        std::ranges::count_if(surfaces, [](const auto& surface) { return surface != nullptr; });
    for (VulkanSwapchain* surface = retainedSurfaces.get(); surface;
         surface = surface->retainedNextForPreparation()) {
      ++owned;
    }
    return surfaceLifetime->liveChildren.load(std::memory_order_acquire) == owned;
  }

  /// Proves ordinary submissions and internal uploads complete without releasing their objects.
  bool prepareSubmissionsForDestruction() {
    for (const InFlightSubmission& submission : inFlight) {
      if (!proveFenceCompleteForTeardown(submission.fence)) {
        return false;
      }
    }
    for (const PendingUpload& upload : pendingUploads) {
      if (!proveFenceCompleteForTeardown(upload.fence)) {
        return false;
      }
    }
    return true;
  }

  /// Waits for \p fence at teardown. A device-lost result proves the work will never run again,
  /// and it is declared to the root, since it may be the first any device over the root saw of
  /// the loss. @param fence Fence of work this device still owns.
  /// @return True when the fence's work is proven complete.
  bool proveFenceCompleteForTeardown(VkFence fence) {
    const VkResult result =
        api->vkWaitForFences(device, 1, &fence, VK_TRUE, kTeardownFenceTimeoutNs);
    if (result == VK_ERROR_DEVICE_LOST) {
      recordDeviceLoss("teardown fence wait reported device loss");
    }
    return CompletionWasProven(result);
  }

  /// Proves every native user complete before any part of the ownership graph is released.
  bool prepareForDestruction() {
    if (!ownsEverySurface()) {
      return false;
    }
    if (device == VK_NULL_HANDLE) {
      return true;
    }
    if (!prepareSubmissionsForDestruction()) {
      return false;
    }
    for (const std::unique_ptr<VulkanSwapchain>& surface : surfaces) {
      if (surface && surface->prepareForDestruction().hasError()) {
        return false;
      }
    }
    for (VulkanSwapchain* surface = retainedSurfaces.get(); surface;
         surface = surface->retainedNextForPreparation()) {
      if (surface->prepareForDestruction().hasError()) {
        return false;
      }
    }
    return ownsEverySurface();
  }

  /// Releases graphics and compute pipelines before their shared layouts.
  void destroyPipelines() {
    for (std::optional<RenderPipelineRecord>& record : renderPipelines) {
      if (record.has_value()) {
        if (record->pipeline != VK_NULL_HANDLE) {
          api->vkDestroyPipeline(device, record->pipeline, nullptr);
        }
        if (record->compatRenderPass != VK_NULL_HANDLE) {
          api->vkDestroyRenderPass(device, record->compatRenderPass, nullptr);
        }
        record.reset();  // Releases the retained pipeline layout.
      }
    }
    renderPipelines.clear();
    for (std::optional<ComputePipelineRecord>& record : computePipelines) {
      if (record.has_value()) {
        if (record->pipeline != VK_NULL_HANDLE) {
          api->vkDestroyPipeline(device, record->pipeline, nullptr);
        }
        record.reset();  // Releases the retained pipeline layout.
      }
    }
    computePipelines.clear();
    pipelineLayouts.clear();  // Releases the remaining VkPipelineLayout handles.
  }

  /// Destroys every remaining native object after the complete graph passed preparation.
  bool teardown() {
    if (!ownsEverySurface()) {
      return false;
    }
    if (device == VK_NULL_HANDLE) {
      destroyDebugMessenger();
      if (instance != VK_NULL_HANDLE) {
        api->vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
      }
      return true;
    }

    for (InFlightSubmission& submission : inFlight) {
      releaseSubmission(submission);
    }
    inFlight.clear();
    for (PendingUpload& upload : pendingUploads) {
      releaseUpload(upload);
    }
    pendingUploads.clear();

    destroyPipelines();
    for (VkShaderModule module : shaderModules) {
      if (module != VK_NULL_HANDLE) {
        api->vkDestroyShaderModule(device, module, nullptr);
      }
    }
    shaderModules.clear();
    for (std::optional<BindGroupRecord>& record : bindGroups) {
      if (record.has_value() && record->pool != VK_NULL_HANDLE) {
        api->vkDestroyDescriptorPool(device, record->pool, nullptr);
      }
    }
    bindGroups.clear();
    for (std::optional<BindGroupLayoutRecord>& record : bindGroupLayouts) {
      if (record.has_value() && record->layout != VK_NULL_HANDLE) {
        api->vkDestroyDescriptorSetLayout(device, record->layout, nullptr);
      }
    }
    bindGroupLayouts.clear();
    for (VkSampler sampler : samplers) {
      if (sampler != VK_NULL_HANDLE) {
        api->vkDestroySampler(device, sampler, nullptr);
      }
    }
    samplers.clear();
    for (std::optional<TextureViewRecord>& record : textureViews) {
      if (record.has_value() && record->view != VK_NULL_HANDLE) {
        api->vkDestroyImageView(device, record->view, nullptr);
      }
    }
    textureViews.clear();
    for (std::optional<TextureRecord>& record : textures) {
      if (record.has_value()) {
        destroyTextureRecord(*record);
      }
    }
    textures.clear();
    for (std::optional<BufferRecord>& record : buffers) {
      if (record.has_value()) {
        destroyBufferRecord(*record);
      }
    }
    buffers.clear();

    // After the views and images above, because a view of a swapchain image must be destroyed
    // before the swapchain that owns the image, and before the command pool and instance below,
    // because a swapchain frees command buffers out of that pool and destroys its surface out of
    // the instance.
    surfaces.clear();
    while (retainedSurfaces) {
      std::unique_ptr<VulkanSwapchain> surface = std::move(retainedSurfaces);
      retainedSurfaces = surface->retainedNext();
    }
    if (surfaceLifetimeUnproven() ||
        surfaceLifetime->liveChildren.load(std::memory_order_acquire) != 0) {
      return false;
    }

    if (commandPool != VK_NULL_HANDLE) {
      api->vkDestroyCommandPool(device, commandPool, nullptr);
      commandPool = VK_NULL_HANDLE;
    }
    api->vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    destroyDebugMessenger();
    if (instance != VK_NULL_HANDLE) {
      api->vkDestroyInstance(instance, nullptr);
      instance = VK_NULL_HANDLE;
    }
    return true;
  }

  // == Presentation ============================================================================

  bool presentationEnabled = false;     //!< Whether swapchain support was requested at creation.
  bool headlessSurfaceEnabled = false;  //!< Whether headless surfaces were enabled with it.

  std::vector<std::unique_ptr<VulkanSwapchain>> surfaces;  //!< Surface slots.
  std::unique_ptr<VulkanSwapchain> retainedSurfaces;       //!< Failed explicit destructions.
  /// Texture slot each surface's acquired frame occupies, empty while it holds none.
  std::vector<std::optional<uint32_t>> surfaceTextureSlots;

  /// The surface at \p slotIndex, or null when that slot holds none.
  /// @param slotIndex Surface slot.
  VulkanSwapchain* surfaceAt(uint32_t slotIndex) const {
    return slotIndex < surfaces.size() ? surfaces[slotIndex].get() : nullptr;
  }

  /// Borrowed objects a swapchain works through.
  VulkanSurfaceContext surfaceContext() const {
    return VulkanSurfaceContext{api,   instance,         physicalDevice, device,
                                queue, queueFamilyIndex, commandPool,    surfaceLifetime};
  }

  /// Waits taken from surfaces for one submission, and the surfaces they came from.
  struct ClaimedSurfaceWaits {
    SurfaceWaitSync sync;                //!< Semaphores and stages for the submission.
    std::vector<uint32_t> surfaceSlots;  //!< Surfaces each semaphore came from, index-matched.
  };

  /// The waits owed by the surfaces whose frames \p usedTextureSlots says this submission writes.
  ///
  /// Only those: a submission that draws into one surface's frame says nothing about when another
  /// surface's frame is safe to write, and sweeping up the second surface's wait here would leave
  /// its own first writer carrying none.
  ///
  /// @param usedTextureSlots Texture slots this submission's recorded commands name.
  ClaimedSurfaceWaits claimSurfaceWaits(const std::vector<uint32_t>& usedTextureSlots) {
    ClaimedSurfaceWaits claimed;
    for (uint32_t surfaceSlot = 0; surfaceSlot < surfaces.size(); ++surfaceSlot) {
      VulkanSwapchain* surface = surfaces[surfaceSlot].get();
      if (surface == nullptr || !surface->frameTextureSlot().has_value()) {
        continue;
      }
      const uint32_t frameSlot = *surface->frameTextureSlot();
      if (std::ranges::find(usedTextureSlots, frameSlot) == usedTextureSlots.end()) {
        continue;
      }

      SurfaceWaitSync wait = surface->takeAcquireWait();
      for (size_t i = 0; i < wait.semaphores.size(); ++i) {
        claimed.sync.semaphores.push_back(wait.semaphores[i]);
        claimed.sync.stages.push_back(wait.stages[i]);
        claimed.surfaceSlots.push_back(surfaceSlot);
      }
    }
    return claimed;
  }

  /// Gives claimed waits back to the surfaces they came from, for a submission the queue refused.
  /// @param claimed Waits taken for that submission.
  void returnSurfaceWaits(const ClaimedSurfaceWaits& claimed) {
    for (size_t i = 0; i < claimed.surfaceSlots.size(); ++i) {
      if (VulkanSwapchain* surface = surfaceAt(claimed.surfaceSlots[i]); surface != nullptr) {
        surface->restoreAcquireWait(claimed.sync.semaphores[i]);
      }
    }
  }

  /// Texture slots the submission being encoded has named, in encounter order with repeats.
  ///
  /// Recorded by \ref transitionTexture, which every texture use passes through, including the
  /// uses whose layout already matches and so record no barrier.
  std::vector<uint32_t> encodedTextureSlots;

  /// Forgets the texture slot a surface's frame occupied, clearing the slot itself only while it
  /// still names \p frameImage - the caller may have destroyed the frame's handle already, and
  /// the runtime hands a released slot to the next texture.
  /// @param slotIndex Surface slot. @param frameImage Image the frame handed out, or null.
  void releaseFrameTextureSlot(uint32_t slotIndex, VkImage frameImage) {
    const std::optional<uint32_t> textureSlot = slotIndex < surfaceTextureSlots.size()
                                                    ? surfaceTextureSlots[slotIndex]
                                                    : std::optional<uint32_t>();
    if (textureSlot.has_value() && frameImage != VK_NULL_HANDLE) {
      if (TextureRecord* record = FindRecord(textures, *textureSlot);
          record != nullptr && record->image == frameImage) {
        textures[*textureSlot].reset();
        syncStates.forget(*textureSlot);
      }
    }
    SetSlot(surfaceTextureSlots, slotIndex, std::optional<uint32_t>());
  }

  /// Mutable state threaded through the encoding of one command stream.
  struct EncodingState {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;  //!< Command buffer being recorded.
    /// Every command buffer allocated for this submission, in execution order; the one being
    /// recorded is the last of them.
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkRenderPass> transientRenderPasses;   //!< Render passes created while encoding.
    std::vector<VkFramebuffer> transientFramebuffers;  //!< Framebuffers created while encoding.
    bool inRenderPass = false;                         //!< True between begin and end pass.
    bool inComputePass = false;                        //!< True between begin and end compute pass.
    BufferRecord bufferWriteStaging;                   //!< Staging released on encoding failure.
    Extent2d passExtent;                               //!< Extent of the active pass.
    const RenderPipelineRecord* currentPipeline = nullptr;  //!< Pipeline bound in the pass.
    /// Compute pipeline bound in the active compute pass.
    const ComputePipelineRecord* currentComputePipeline = nullptr;
    /// Groups bound by index; their records remain live throughout command encoding.
    std::array<const BindGroupRecord*, kMaxBindGroups> boundGroups = {};
    /// Recorded index binding whose native bind waits for the first nonzero indexed draw.
    struct PendingIndexBinding {
      VkBuffer buffer = VK_NULL_HANDLE;              //!< Buffer to bind.
      VkDeviceSize offsetBytes = 0;                  //!< Byte offset of the first index.
      VkIndexType indexType = VK_INDEX_TYPE_UINT16;  //!< Width of each index.
    };
    /// vkCmdBindIndexBuffer requires offset < buffer size, but the encoder accepts a binding
    /// exactly at the end as an empty range; only a nonzero draw proves the range nonempty.
    std::optional<PendingIndexBinding> pendingIndexBinding;
  };

  /// Packs and records pending writes before the command stream without consuming the queue.
  /// @param state Encoding state that owns the staging buffer until submission succeeds.
  Status encodePendingBufferWrites(EncodingState& state);

  /// Records the barrier that puts \p textureSlot into \p usage, if one is needed, and stages
  /// the state it leaves behind.
  ///
  /// A barrier is recorded when the layout changes, and also when the image was last written
  /// even though its layout does not change: two compute passes writing the same storage texture
  /// stay in one layout throughout and still need the second to wait for the first.
  ///
  /// @param commandBuffer Command buffer to record into.
  /// @param textureSlot Texture slot being transitioned.
  /// @param record Texture record backing the slot.
  /// @param usage Usage the texture is about to be put to.
  void transitionTexture(VkCommandBuffer commandBuffer, uint32_t textureSlot,
                         const TextureRecord& record, TextureUsageKind usage);

  /// Destroys the render passes, framebuffers, and command buffer created for an encoding that
  /// will not be submitted.
  /// @param state Encoding state.
  void destroyTransientEncodingObjects(EncodingState& state);

  /// Releases an encoding whose queue ownership has been excluded or drained.
  void discardEncoding(EncodingState& state) {
    syncStates.discardStaged();
    destroyTransientEncodingObjects(state);
    destroyBufferRecord(state.bufferWriteStaging);
  }

  /// Allocates and records one command buffer of a submission, leaving it unsubmitted.
  /// @param state Encoding state the buffer is appended to.
  /// @param commands Commands to record, in recording order.
  /// @param encodeQueuedWrites Whether to record the queued writes ahead of \p commands.
  Status encodeSubmittedCommandBuffer(EncodingState& state, std::span<const Command> commands,
                                      bool encodeQueuedWrites);

  /// Transfers encoded objects to queue ownership or frees a proven rejected submission.
  Status finishSubmission(uint64_t submissionSerial, EncodingState& state, VkFence fence);

  /// Transitions the textures in one bind group to their descriptor layouts.
  /// @param state Encoding state.
  /// @param group Bind group whose textures are about to be used.
  void transitionBoundTextures(EncodingState& state, const BindGroupRecord& group);

  /// Transitions every texture the upcoming render pass binds to its descriptor layout:
  /// sampled textures to SHADER_READ_ONLY_OPTIMAL and storage textures to GENERAL. These layout
  /// changes must precede vkCmdBeginRenderPass, so the render commands are scanned in advance.
  /// @param state Encoding state.
  /// @param commands Full command stream.
  /// @param beginIndex Index of the begin-pass command.
  void transitionRenderPassBoundTextures(EncodingState& state, std::span<const Command> commands,
                                         size_t beginIndex);

  /// Creates the pass's render pass and framebuffer, transitions its attachments, and begins it
  /// with the WebGPU-style full-attachment viewport and scissor.
  /// @param state Encoding state.
  /// @param beginPass Recorded begin-pass command.
  Status beginEncodedRenderPass(EncodingState& state, const BeginRenderPassCommand& beginPass);

  /// Binds a recorded pipeline.
  /// @param state Encoding state.
  /// @param setPipeline Recorded command.
  Status encodeSetPipeline(EncodingState& state, const SetPipelineCommand& setPipeline);

  /// Records a recorded bind group for lazy binding at draw.
  /// @param state Encoding state.
  /// @param setBindGroup Recorded command.
  Status encodeSetBindGroup(EncodingState& state, const SetBindGroupCommand& setBindGroup);

  /// Binds a recorded vertex buffer.
  /// @param state Encoding state.
  /// @param setVertexBuffer Recorded command.
  Status encodeSetVertexBuffer(EncodingState& state, const SetVertexBufferCommand& setVertexBuffer);

  /// Records an index binding; the native bind is issued by the first nonzero indexed draw.
  /// @param state Encoding state.
  /// @param setIndexBuffer Recorded command.
  Status encodeSetIndexBuffer(EncodingState& state, const SetIndexBufferCommand& setIndexBuffer);

  /// Sets an explicit scissor rectangle.
  /// @param state Encoding state.
  /// @param setScissor Recorded command.
  Status encodeSetScissorRect(EncodingState& state, const SetScissorRectCommand& setScissor);

  /// Sets an explicit viewport, keeping the pass's y-flip.
  /// @param state Encoding state.
  /// @param setViewport Recorded command.
  Status encodeSetViewport(EncodingState& state, const SetViewportCommand& setViewport);

  /// Binds every descriptor set the pipeline layout declares and issues the draw.
  /// @param state Encoding state.
  /// @param draw Recorded command.
  Status encodeDraw(EncodingState& state, const DrawCommand& draw);

  /// Binds every descriptor set the pipeline layout declares and issues the indexed draw.
  /// @param state Encoding state.
  /// @param draw Recorded command.
  Status encodeDrawIndexed(EncodingState& state, const DrawIndexedCommand& draw);

  /// Binds every descriptor set the active pipeline layout declares, shared by both draws.
  /// @param state Encoding state.
  /// @param operation Operation name for diagnostics.
  Status bindDrawDescriptorSets(EncodingState& state, std::string_view operation);

  /// Ends the active render pass and resets the per-pass binding state.
  /// @param state Encoding state.
  Status encodeEndRenderPass(EncodingState& state);

  /// Records a texture-to-buffer copy plus the barriers that make it visible to host reads.
  /// @param state Encoding state.
  /// @param copy Recorded command.
  Status encodeCopyTextureToBuffer(EncodingState& state, const CopyTextureToBufferCommand& copy);
  Status encodeCopyTextureToTexture(EncodingState& state, const CopyTextureToTextureCommand& copy);

  /// Encodes a render-pass command, or returns empty when the command is not one.
  /// @param state Encoding state.
  /// @param commands Full command stream, for the begin-pass pre-scan.
  /// @param commandIndex Index of the command to encode.
  std::optional<Status> encodeRenderCommand(EncodingState& state, std::span<const Command> commands,
                                            size_t commandIndex);

  /// Opens a compute pass; the barriers its bindings need were recorded before it began.
  /// @param state Encoding state.
  Status beginEncodedComputePass(EncodingState& state);

  /// Binds a recorded compute pipeline.
  /// @param state Encoding state. @param setPipeline Recorded command.
  Status encodeSetComputePipeline(EncodingState& state,
                                  const SetComputePipelineCommand& setPipeline);

  /// Binds every descriptor set the compute pipeline layout declares and issues the dispatch.
  /// @param state Encoding state. @param dispatch Recorded command.
  Status encodeDispatchWorkgroups(EncodingState& state, const DispatchWorkgroupsCommand& dispatch);

  /// Ends the compute pass, making its shader writes visible to everything that follows.
  /// @param state Encoding state.
  Status encodeEndComputePass(EncodingState& state);

  /// Encodes a compute-pass command, or returns empty when the command is not one.
  /// @param state Encoding state.
  /// @param commands Full command stream, for the begin-pass pre-scan.
  /// @param commandIndex Index of the command to encode.
  std::optional<Status> encodeComputeCommand(EncodingState& state,
                                             std::span<const Command> commands,
                                             size_t commandIndex);

  /// Encodes a copy command, or returns empty when \p command is not one.
  /// @param state Encoding state. @param command Recorded command.
  std::optional<Status> encodeCopyCommand(EncodingState& state, const Command& command);

  /// Encodes one recorded command.
  /// @param state Encoding state.
  /// @param commands Full command stream, for the begin-pass pre-scan.
  /// @param commandIndex Index of the command to encode.
  Status encodeCommand(EncodingState& state, std::span<const Command> commands,
                       size_t commandIndex);

  /// Encodes a complete validated stream, preserving the first failure.
  /// @param state Per-submission encoding state.
  /// @param commands Commands to encode in order.
  Status encodeCommands(EncodingState& state, std::span<const Command> commands);

  /// The synchronization state of every live texture, staged during an encode and committed
  /// only once its submission reached the queue.
  TextureSyncStateTable syncStates;

  /// Opt-in image barrier history. Disengaged during normal device use.
  std::optional<std::vector<RecordedImageBarrierForTest>> recordedBarriers;
  /// Set by the test seam; makes the next internal upload report failure at the named point.
  std::optional<UploadFailureModeForTest> uploadFailureMode;

  /// Destroys the transient objects of a staged upload, in reverse creation order.
  /// @param fence Upload fence, or null.
  /// @param commandBuffer Upload command buffer, or null.
  /// @param staging Staging buffer record.
  void destroyUploadObjects(VkFence fence, VkCommandBuffer commandBuffer, BufferRecord& staging);

  /// Records the staged buffer-to-image copy plus the transitions around it, staging the state
  /// they leave the image in.
  /// @param commandBuffer Command buffer to record into.
  /// @param textureSlot Slot of the destination texture.
  /// @param texture Destination texture record.
  /// @param stagingBuffer Buffer holding the upload bytes.
  /// @param dataLayout Row layout of the upload bytes.
  /// @param writeSize Destination extent in texels.
  /// @param destinationOrigin Top-left destination texel the copy writes to.
  void recordTextureUploadCopy(VkCommandBuffer commandBuffer, uint32_t textureSlot,
                               const TextureRecord& texture, VkBuffer stagingBuffer,
                               const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize,
                               const Origin2d& destinationOrigin);

  /// Retains accepted upload objects on an unproven wait, otherwise releases them.
  /// @param slotIndex Destination texture slot. @param commandBuffer Recorded upload.
  /// @param fence Receives the upload fence. @param staging Upload staging allocation.
  /// @param reachedQueue Receives whether committed image transitions must be preserved.
  Status finishTextureUpload(uint32_t slotIndex, VkCommandBuffer commandBuffer, VkFence& fence,
                             BufferRecord& staging, bool& reachedQueue);

  /// Creates \p fence, submits \p commandBuffer on it, and waits for completion.
  /// @param commandBuffer Recorded upload command buffer. @param fence Receives the created fence.
  /// @param objectsStillInUse Receives whether upload objects must remain alive.
  /// @param reachedQueue Receives whether the queue accepted the upload.
  Status submitAndWaitTextureUpload(VkCommandBuffer commandBuffer, VkFence& fence,
                                    bool& objectsStillInUse, bool& reachedQueue);

  /// Waits for the upload fence \p fence. The wait ends early when a device over the root
  /// declares the root lost, and anything but a signalled fence leaves the upload's objects owned.
  /// @param fence Fence of the submitted upload.
  /// @param objectsStillInUse Set when the upload's objects must remain alive.
  Status waitForTextureUpload(VkFence fence, bool& objectsStillInUse);

  /// Destroys the Vulkan buffer backing \p slotIndex, if any.
  /// @param slotIndex Buffer slot.
  void destroyBufferSlot(uint32_t slotIndex);

  /// Dispatches native resource destruction after the caller proves ownership permits it.
  void destroyResourceSlot(std::string_view resourceName, uint32_t slotIndex);
  /// Destroys the Vulkan image backing \p slotIndex, if any.
  /// @param slotIndex Texture slot.
  void destroyTextureSlot(uint32_t slotIndex);
  /// Destroys the Vulkan image view backing \p slotIndex, if any.
  /// @param slotIndex Texture view slot.
  void destroyTextureViewSlot(uint32_t slotIndex);
  /// Destroys the Vulkan sampler backing \p slotIndex, if any.
  /// @param slotIndex Sampler slot.
  void destroySamplerSlot(uint32_t slotIndex);
  /// Destroys the Vulkan descriptor set layout backing \p slotIndex, if any.
  /// @param slotIndex Bind group layout slot.
  void destroyBindGroupLayoutSlot(uint32_t slotIndex);
  /// Destroys the descriptor pool backing \p slotIndex, which frees its descriptor set.
  /// @param slotIndex Bind group slot.
  void destroyBindGroupSlot(uint32_t slotIndex);
  /// Drops the slot's reference to the shared Vulkan pipeline layout.
  /// @param slotIndex Pipeline layout slot.
  void destroyPipelineLayoutSlot(uint32_t slotIndex);
  /// Destroys the Vulkan shader module backing \p slotIndex, if any.
  /// @param slotIndex Shader module slot.
  void destroyShaderModuleSlot(uint32_t slotIndex);
  /// Destroys the Vulkan pipeline and compatibility render pass backing \p slotIndex, if any.
  /// @param slotIndex Render pipeline slot.
  void destroyRenderPipelineSlot(uint32_t slotIndex);
  /// Destroys the Vulkan compute pipeline backing \p slotIndex, if any.
  /// @param slotIndex Compute pipeline slot.
  void destroyComputePipelineSlot(uint32_t slotIndex);

  /// Resolves both shader modules a render pipeline references. Returns false when either slot
  /// has no Vulkan module.
  /// @param descriptor Render pipeline descriptor.
  /// @param vertexModule Set to the vertex stage module on success.
  /// @param fragmentModule Set to the fragment stage module on success.
  bool lookupShaderModules(const RenderPipelineDescriptor& descriptor, VkShaderModule& vertexModule,
                           VkShaderModule& fragmentModule) const {
    const uint32_t vertexModuleSlot = descriptor.vertex.module.slotIndex();
    const uint32_t fragmentModuleSlot = descriptor.fragment.module.slotIndex();
    vertexModule =
        vertexModuleSlot < shaderModules.size() ? shaderModules[vertexModuleSlot] : VK_NULL_HANDLE;
    fragmentModule = fragmentModuleSlot < shaderModules.size() ? shaderModules[fragmentModuleSlot]
                                                               : VK_NULL_HANDLE;
    return vertexModule != VK_NULL_HANDLE && fragmentModule != VK_NULL_HANDLE;
  }

  /// Resolves the Vulkan object one bind group entry references and points \p write at the
  /// descriptor info describing it. Fails closed when the entry does not resolve.
  /// @param entry Bind group entry to resolve.
  /// @param index Entry index, selecting the info slot backing \p write.
  /// @param bufferInfos Storage for buffer descriptor infos, indexed by entry.
  /// @param imageInfos Storage for image descriptor infos, indexed by entry.
  /// @param record Bind group record receiving the texture slots the entry references.
  /// @param write Descriptor write to point at the resolved info, carrying the descriptor type.
  Status bindDescriptorResource(const BindGroupEntry& entry, size_t index,
                                std::vector<VkDescriptorBufferInfo>& bufferInfos,
                                std::vector<VkDescriptorImageInfo>& imageInfos,
                                BindGroupRecord& record, VkWriteDescriptorSet& write);
};

std::unique_ptr<VulkanDevice> VulkanDevice::Create(std::shared_ptr<DeviceLostState> lostState) {
  return CreateImpl(false, false, {}, std::move(lostState));
}

std::unique_ptr<VulkanDevice> VulkanDevice::CreateWithTimelineSemaphoreForTest(
    std::shared_ptr<DeviceLostState> lostState) {
  return CreateImpl(true, false, {}, std::move(lostState));
}

std::unique_ptr<VulkanDevice> VulkanDevice::CreateWithPresentationSupport(
    std::span<const char* const> requiredInstanceExtensions,
    std::shared_ptr<DeviceLostState> lostState) {
  return CreateImpl(false, true, requiredInstanceExtensions, std::move(lostState));
}

std::unique_ptr<VulkanDevice> VulkanDevice::CreateImpl(
    bool enableTimelineSemaphoreForTest, bool enablePresentation,
    std::span<const char* const> requiredInstanceExtensions,
    std::shared_ptr<DeviceLostState> lostState) {
  Impl::AdmissionGate& gate = Impl::admissionGate();
  const std::lock_guard admission(gate.mutex);
  if (gate.closed) {
    return nullptr;
  }
  InstanceSetup setup = CreateInstance(enablePresentation, requiredInstanceExtensions);
  if (setup.loader == nullptr) {
    return nullptr;
  }
  const std::shared_ptr<VulkanLoader>& loader = setup.loader;
  const VulkanApi& api = loader->api();
  const VkInstance instance = setup.instance;

  // First enumerated physical device with 1.1 support and a graphics queue family.
  VkPhysicalDevice selectedDevice = VK_NULL_HANDLE;
  uint32_t selectedQueueFamily = 0;
  if (!SelectGraphicsPhysicalDevice(api, instance, selectedDevice, selectedQueueFamily)) {
    api.vkDestroyInstance(instance, nullptr);
    return nullptr;
  }

  bool fullDrawIndexUint32 = false;
  const VkDevice device =
      CreateLogicalDevice(api, selectedDevice, selectedQueueFamily, setup.presentationExtensions,
                          enableTimelineSemaphoreForTest, enablePresentation, fullDrawIndexUint32);
  if (device == VK_NULL_HANDLE) {
    api.vkDestroyInstance(instance, nullptr);
    return nullptr;
  }

  // Device-level entry points come from the device itself, so every call this backend records
  // per frame reaches the implementation directly instead of through the loader's dispatch.
  if (const Status status = LoadDeviceEntryPoints(*loader, device, enablePresentation);
      status.hasError()) {
    std::fprintf(stderr, "[donner::gpu::vulkan] %s\n", status.error().message.c_str());
    if (api.vkDestroyDevice != nullptr) {
      api.vkDestroyDevice(device, nullptr);
    }
    api.vkDestroyInstance(instance, nullptr);
    return nullptr;
  }

  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = selectedQueueFamily;
  if (api.vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
    api.vkDestroyDevice(device, nullptr);
    api.vkDestroyInstance(instance, nullptr);
    return nullptr;
  }

  std::unique_ptr<VulkanDevice> result(new VulkanDevice());
  Impl& impl = *result->impl_;
  if (lostState) {
    impl.rootLoss = lostState;
    result->adoptLostState(std::move(lostState));
  }
  impl.loader = loader;
  impl.api = &loader->api();
  impl.instance = instance;
  impl.physicalDevice = selectedDevice;
  impl.device = device;
  impl.queueFamilyIndex = selectedQueueFamily;
  impl.commandPool = commandPool;
  impl.fullDrawIndexUint32 = fullDrawIndexUint32;
  impl.presentationEnabled = enablePresentation;
  impl.headlessSurfaceEnabled = setup.headlessSurfaceAvailable;
  api.vkGetDeviceQueue(device, selectedQueueFamily, 0, &impl.queue);
  api.vkGetPhysicalDeviceMemoryProperties(selectedDevice, &impl.memoryProperties);
  impl.bufferAllocator = std::make_unique<DedicatedBufferAllocator>(impl.memoryProperties);

  if (setup.debugMessengerAvailable) {
    const DebugMessengerHandles messenger =
        CreateValidationMessenger(api, instance, impl.errorState.get());
    impl.destroyDebugMessengerFn = messenger.destroyFn;
    impl.debugMessenger = messenger.messenger;
  }
  return result;
}

VulkanDevice::VulkanDevice() : impl_(std::make_unique<Impl>()) {
  adoptLostState(impl_->rootLoss);
}

std::unique_ptr<VulkanDevice> VulkanDevice::CreateForTeardownTest(
    const VulkanApi* api, uint64_t instanceHandle, uint64_t deviceHandle,
    uint64_t commandPoolHandle, void (*onAdmission)(void*), void* admissionContext,
    std::shared_ptr<DeviceLostState> lostState) {
  Impl::AdmissionGate& gate = Impl::admissionGate();
  const std::lock_guard admission(gate.mutex);
  if (gate.closed) {
    return nullptr;
  }
  if (onAdmission) {
    onAdmission(admissionContext);
  }
  std::unique_ptr<VulkanDevice> result(new VulkanDevice());
  if (lostState) {
    result->impl_->rootLoss = lostState;
    result->adoptLostState(std::move(lostState));
  }
  result->impl_->testApi = *api;
  result->impl_->api = &*result->impl_->testApi;
  static_assert(sizeof(result->impl_->instance) == sizeof(instanceHandle));
  static_assert(sizeof(result->impl_->device) == sizeof(deviceHandle));
  static_assert(sizeof(result->impl_->commandPool) == sizeof(commandPoolHandle));
  std::memcpy(&result->impl_->instance, &instanceHandle, sizeof(instanceHandle));
  std::memcpy(&result->impl_->device, &deviceHandle, sizeof(deviceHandle));
  std::memcpy(&result->impl_->commandPool, &commandPoolHandle, sizeof(commandPoolHandle));
  return result;
}

bool VulkanDevice::CreateNativeObjectsForPresentationTest(
    const VulkanApi& api, bool enablePresentation,
    std::span<const char* const> requiredInstanceExtensions) {
  Impl::AdmissionGate& gate = Impl::admissionGate();
  const std::lock_guard admission(gate.mutex);
  if (gate.closed) {
    return false;
  }
  bool debugUtilsEnabled = false;
  std::vector<const char*> instanceExtensions;
  const VkInstance instance = CreateNativeInstance(
      api, enablePresentation, requiredInstanceExtensions, debugUtilsEnabled, instanceExtensions);
  if (instance == VK_NULL_HANDLE) {
    return false;
  }
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkDevice device = VK_NULL_HANDLE;
  if (SelectGraphicsPhysicalDevice(api, instance, physicalDevice, queueFamily)) {
    bool fullDrawIndexUint32 = false;
    device = CreateLogicalDevice(api, physicalDevice, queueFamily, instanceExtensions, false,
                                 enablePresentation, fullDrawIndexUint32);
  }
  if (device != VK_NULL_HANDLE) {
    api.vkDestroyDevice(device, nullptr);
  }
  api.vkDestroyInstance(instance, nullptr);
  return device != VK_NULL_HANDLE;
}

void VulkanDevice::attachSurfaceForTeardownTest(std::unique_ptr<VulkanSwapchain> surface) {
  impl_->surfaces.push_back(std::move(surface));
}

void VulkanDevice::attachWorkForTeardownTest(bool upload, uint64_t fenceHandle,
                                             uint64_t commandBufferHandle) {
  VkFence fence;
  VkCommandBuffer commandBuffer;
  static_assert(sizeof(fence) == sizeof(fenceHandle));
  static_assert(sizeof(commandBuffer) == sizeof(commandBufferHandle));
  std::memcpy(&fence, &fenceHandle, sizeof(fence));
  std::memcpy(&commandBuffer, &commandBufferHandle, sizeof(commandBuffer));
  if (upload) {
    impl_->pendingUploads.push_back({fence, commandBuffer, {}, 0, std::nullopt});
  } else {
    Impl::InFlightSubmission submission;
    submission.serial = impl_->inFlight.size() + 1;
    submission.fence = fence;
    submission.commandBuffers.push_back(commandBuffer);
    impl_->inFlight.push_back(std::move(submission));
  }
}

VulkanSurfaceContext VulkanDevice::surfaceContextForTeardownTest() const {
  return impl_->surfaceContext();
}

VulkanDevice::~VulkanDevice() {
  // A surface can outlive this object when teardown cannot prove its work finished, so none may
  // call back into it from here on.
  for (const std::unique_ptr<VulkanSwapchain>& surface : impl_->surfaces) {
    if (surface != nullptr) {
      surface->setQueueSubmissionCallback({});
    }
  }
  Impl::AdmissionGate& gate = Impl::admissionGate();
  const std::lock_guard admission(gate.mutex);
  if (!impl_->prepareForDestruction() || !impl_->teardown()) {
    Impl::quarantineUnderAdmissionLock(std::move(impl_), gate);
    std::fprintf(stderr,
                 "[donner::gpu::vulkan] shutdown incomplete; Vulkan disabled until restart\n");
    return;
  }
}

VulkanDevice::BufferWriteStats VulkanDevice::bufferWriteStatsForTest() const {
  size_t retiredBuffers = 0;
  for (const Impl::InFlightSubmission& submission : impl_->inFlight) {
    retiredBuffers += submission.retiredBuffers.size();
  }
  return {impl_->pendingBufferWrites.size(),
          impl_->pendingBufferWriteBytes,
          impl_->inFlightBufferWriteBytes,
          impl_->bufferWriteStagingAllocations,
          impl_->submittedBufferWriteBatches,
          retiredBuffers,
          impl_->lostDeviceDrains};
}

void VulkanDevice::setBufferWriteByteBudgetForTest(uint64_t byteBudget) {
  impl_->bufferWriteByteBudget = std::min(byteBudget, kMaxBufferByteSize);
}

void VulkanDevice::failNextSubmissionForTest(bool deviceLost) {
  impl_->nextSubmissionFailure = deviceLost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void VulkanDevice::setFenceWaitStepHookForTest(std::function<void()> hook) {
  impl_->fenceWaitStepHookForTest = std::move(hook);
}

VulkanDevice::NativeContextForTest VulkanDevice::nativeContextForTest() const {
  return {impl_->api, impl_->instance, impl_->device, impl_->queue, impl_->queueFamilyIndex};
}

uint64_t VulkanDevice::completedSerial() const {
  // Single-threaded device (see the class comment): polling from a const accessor is safe, and
  // unique_ptr does not propagate const to the Impl.
  impl_->pollCompleted();
  return impl_->completedSerialValue;
}

bool VulkanDevice::onWaitForSerial(uint64_t serial, double timeoutSeconds) {
  Impl& impl = *impl_;
  impl.pollCompleted();
  if (impl.hasError()) {
    return false;
  }
  if (impl.completedSerialValue >= serial) {
    return true;
  }
  // A root that any device over it declared lost will not finish this work: say so now rather
  // than spend the budget, which is also what keeps a caller from recording its own timeout.
  if (isLost()) {
    return false;
  }

  const Impl::InFlightSubmission* target = impl.firstInFlightThrough(serial);
  if (target == nullptr) {
    return false;  // Serial was never submitted.
  }
  const Impl::FenceWait waited = impl.waitForFenceUnlessLost(target->fence, timeoutSeconds);
  if (waited.end == Impl::FenceWaitEnd::Failed) {
    impl.recordFenceWaitFailure(waited.result);
  }
  if (waited.end != Impl::FenceWaitEnd::Signalled) {
    return false;
  }
  impl.pollCompleted();
  return !impl.hasError() && impl.completedSerialValue >= serial;
}

Status VulkanDevice::waitForBufferAccess(uint64_t serial, std::string_view operation) {
  if (waitForSerial(serial, kBusyBufferAccessTimeoutSeconds)) {
    return OkStatus();
  }
  const std::string error = lastErrorForTest();
  // A loss another device over the root declared leaves no error here; it is still a loss, not
  // a timeout. An error of this device's own, a driver-reported loss included, is reported as is.
  if (error.empty() && isLost()) {
    return GpuError{
        GpuErrorType::DeviceLost,
        std::format("{} cannot wait for submission {}: the device was lost", operation, serial)};
  }
  return GpuError{GpuErrorType::InvalidState,
                  error.empty()
                      ? std::format("{} timed out waiting for submission {}", operation, serial)
                      : error};
}

Result<std::vector<uint8_t>> VulkanDevice::readBackBuffer(const Buffer& buffer) {
  // Full handle validation (null, device identity, AND generation) through the base class, so a
  // stale handle whose slot was reused cannot read the replacement buffer.
  if (Status status = validateBufferHandleForBackend(buffer); status.hasError()) {
    return std::move(status).error();
  }
  const Impl::BufferRecord* record = FindRecord(impl_->buffers, buffer.slotIndex());
  if (record == nullptr || record->allocation.mapped == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("buffer handle (slot {}) does not name a live Vulkan buffer",
                                buffer.slotIndex())};
  }

  const uint64_t lastUse = std::max(bufferLastUseSerial(buffer.slotIndex()), record->uploadSerial);
  if (Status status = waitForBufferAccess(lastUse, "readBackBuffer"); status.hasError()) {
    return std::move(status).error();
  }

  const uint8_t* contents = static_cast<const uint8_t*>(record->allocation.mapped);
  return std::vector<uint8_t>(contents, contents + record->byteSize);
}

Result<VulkanDevice::TrackedTextureLayout> VulkanDevice::trackedTextureLayoutForTest(
    const Texture& texture) const {
  if (Status status = validateTextureHandleForBackend(texture); status.hasError()) {
    return std::move(status).error();
  }
  const Impl::TextureRecord* record = FindRecord(impl_->textures, texture.slotIndex());
  if (record == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("texture handle (slot {}) does not name a live Vulkan image",
                                texture.slotIndex())};
  }
  switch (impl_->syncStates.stateOf(texture.slotIndex()).layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return TrackedTextureLayout::Undefined;
    case VK_IMAGE_LAYOUT_GENERAL: return TrackedTextureLayout::General;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return TrackedTextureLayout::ShaderReadOnly;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return TrackedTextureLayout::TransferSrc;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return TrackedTextureLayout::TransferDst;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return TrackedTextureLayout::ColorAttachment;
    default: return TrackedTextureLayout::Other;
  }
}

void VulkanDevice::setImageBarrierRecordingForTest(bool enabled) {
  if (enabled) {
    impl_->recordedBarriers.emplace();
  } else {
    impl_->recordedBarriers.reset();
  }
}

std::vector<VulkanDevice::RecordedImageBarrierForTest> VulkanDevice::recordedImageBarriersForTest()
    const {
  return impl_->recordedBarriers.value_or(std::vector<RecordedImageBarrierForTest>{});
}

void VulkanDevice::failNextTextureUploadForTest(UploadFailureModeForTest mode) {
  impl_->uploadFailureMode = mode;
}

size_t VulkanDevice::pendingTextureUploadCountForTest() const {
  return impl_->pendingUploads.size();
}

void VulkanDevice::deferTextureUploadPollingForTest(bool defer) {
  impl_->deferUploadPolling = defer;
}

bool VulkanDevice::supportsFullIndexRange(IndexFormat format) const {
  return format != IndexFormat::Uint32 || impl_->fullDrawIndexUint32;
}

void VulkanDevice::disableFullUint32IndexRangeForTest() {
  impl_->fullDrawIndexUint32 = false;
}

std::string VulkanDevice::lastErrorForTest() const {
  return impl_->errorMessage();
}

Status VulkanDevice::onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Result<Impl::BufferRecord> record = impl_->createHostVisibleBuffer(
      descriptor.byteSize, ToVkBufferUsage(descriptor.usage), std::string_view(descriptor.label));
  if (record.hasError()) {
    return std::move(record).error();
  }
  SetSlot(impl_->buffers, slotIndex, std::optional<Impl::BufferRecord>(std::move(record).result()));
  return OkStatus();
}

Status VulkanDevice::onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Impl& impl = *impl_;
  Impl::TextureRecord record;
  record.format = descriptor.format;
  record.size = descriptor.size;
  record.usage = descriptor.usage;

  VkImageCreateInfo imageInfo = {};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = ToVkFormat(descriptor.format);
  imageInfo.extent = VkExtent3D{descriptor.size.width, descriptor.size.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = ToVkImageUsage(descriptor.usage);
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (const VkResult result =
          impl_->api->vkCreateImage(impl.device, &imageInfo, nullptr, &record.image);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkCreateImage ({}x{}) for '{}' failed with {}",
                                descriptor.size.width, descriptor.size.height,
                                std::string_view(descriptor.label), VkResultToString(result))};
  }

  VkMemoryRequirements requirements = {};
  impl_->api->vkGetImageMemoryRequirements(impl.device, record.image, &requirements);
  // Prefer device-local image memory; fall back to any compatible type (software
  // implementations may expose a single unified heap).
  std::optional<uint32_t> memoryType = FindMemoryType(
      impl.memoryProperties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!memoryType) {
    memoryType = FindMemoryType(impl.memoryProperties, requirements.memoryTypeBits, 0);
  }
  if (!memoryType) {
    impl.destroyTextureRecord(record);
    return GpuError{GpuErrorType::InvalidState,
                    std::format("no compatible memory type for texture '{}'",
                                std::string_view(descriptor.label))};
  }

  VkMemoryAllocateInfo allocateInfo = {};
  allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocateInfo.allocationSize = requirements.size;
  allocateInfo.memoryTypeIndex = *memoryType;
  if (const VkResult result =
          impl_->api->vkAllocateMemory(impl.device, &allocateInfo, nullptr, &record.memory);
      result != VK_SUCCESS) {
    impl.destroyTextureRecord(record);
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkAllocateMemory of {} bytes for texture '{}' failed with {}",
                                requirements.size, std::string_view(descriptor.label),
                                VkResultToString(result))};
  }
  if (const VkResult result =
          impl_->api->vkBindImageMemory(impl.device, record.image, record.memory, 0);
      result != VK_SUCCESS) {
    impl.destroyTextureRecord(record);
    return VkError("vkBindImageMemory", result);
  }

  SetSlot(impl.textures, slotIndex, std::optional<Impl::TextureRecord>(std::move(record)));
  return OkStatus();
}

Status VulkanDevice::onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                         const TextureViewDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  const Impl::TextureRecord* texture = FindRecord(impl_->textures, textureSlotIndex);
  if (texture == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no Vulkan image", textureSlotIndex)};
  }

  VkImageViewCreateInfo viewInfo = {};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = texture->image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = ToVkFormat(texture->format);
  viewInfo.subresourceRange = FullColorRange();
  VkImageView view = VK_NULL_HANDLE;
  if (const VkResult result =
          impl_->api->vkCreateImageView(impl_->device, &viewInfo, nullptr, &view);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkCreateImageView for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }

  SetSlot(impl_->textureViews, slotIndex,
          std::optional<Impl::TextureViewRecord>(Impl::TextureViewRecord{view, textureSlotIndex}));
  return OkStatus();
}

Status VulkanDevice::onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  VkSamplerCreateInfo samplerInfo = {};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = ToVkFilter(descriptor.magFilter);
  samplerInfo.minFilter = ToVkFilter(descriptor.minFilter);
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = ToVkAddressMode(descriptor.addressModeU);
  samplerInfo.addressModeV = ToVkAddressMode(descriptor.addressModeV);
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
  samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;

  VkSampler sampler = VK_NULL_HANDLE;
  if (const VkResult result =
          impl_->api->vkCreateSampler(impl_->device, &samplerInfo, nullptr, &sampler);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkCreateSampler for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }
  SetSlot(impl_->samplers, slotIndex, sampler);
  return OkStatus();
}

Status VulkanDevice::onCreateBindGroupLayout(uint32_t slotIndex,
                                             const BindGroupLayoutDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  bindings.reserve(descriptor.entries.size());
  for (const BindGroupLayoutEntry& entry : descriptor.entries) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = entry.binding;
    binding.descriptorType = ToVkDescriptorType(entry.type);
    binding.descriptorCount = 1;
    binding.stageFlags = ToVkShaderStages(entry.visibility);
    bindings.push_back(binding);
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo = {};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (const VkResult result =
          impl_->api->vkCreateDescriptorSetLayout(impl_->device, &layoutInfo, nullptr, &layout);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkCreateDescriptorSetLayout for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }
  SetSlot(
      impl_->bindGroupLayouts, slotIndex,
      std::optional<Impl::BindGroupLayoutRecord>(Impl::BindGroupLayoutRecord{layout, descriptor}));
  return OkStatus();
}

Status VulkanDevice::Impl::bindDescriptorResource(const BindGroupEntry& entry, size_t index,
                                                  std::vector<VkDescriptorBufferInfo>& bufferInfos,
                                                  std::vector<VkDescriptorImageInfo>& imageInfos,
                                                  BindGroupRecord& record,
                                                  VkWriteDescriptorSet& write) {
  if (const BufferBinding* bufferBinding = std::get_if<BufferBinding>(&entry.resource)) {
    const BufferRecord* buffer = FindRecord(buffers, bufferBinding->buffer.slotIndex());
    if (buffer == nullptr) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("bind group binding {} does not resolve to a Vulkan "
                                  "buffer",
                                  entry.binding)};
    }
    bufferInfos[index] = VkDescriptorBufferInfo{buffer->buffer, bufferBinding->offsetBytes,
                                                bufferBinding->sizeBytes};
    write.pBufferInfo = &bufferInfos[index];
  } else if (const TextureViewBinding* viewBinding =
                 std::get_if<TextureViewBinding>(&entry.resource)) {
    const TextureViewRecord* view = FindRecord(textureViews, viewBinding->view.slotIndex());
    if (view == nullptr) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("bind group binding {} does not resolve to a Vulkan "
                                  "image view",
                                  entry.binding)};
    }
    // A storage image is written through, so its descriptor declares GENERAL; a sampled image
    // declares the read-only layout. Either way the slot is snapshotted so onSubmit can
    // pre-transition it before a pass that binds this group.
    const bool storageImage = write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkImageLayout declaredLayout =
        storageImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[index] = VkDescriptorImageInfo{VK_NULL_HANDLE, view->view, declaredLayout};
    write.pImageInfo = &imageInfos[index];
    if (storageImage) {
      record.storageTextureSlots.push_back(view->textureSlot);
    } else {
      record.sampledTextureSlots.push_back(view->textureSlot);
    }
  } else if (const SamplerBinding* samplerBinding = std::get_if<SamplerBinding>(&entry.resource)) {
    const uint32_t samplerSlot = samplerBinding->sampler.slotIndex();
    const VkSampler sampler =
        samplerSlot < samplers.size() ? samplers[samplerSlot] : VK_NULL_HANDLE;
    if (sampler == VK_NULL_HANDLE) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("bind group binding {} does not resolve to a Vulkan "
                                  "sampler",
                                  entry.binding)};
    }
    imageInfos[index] = VkDescriptorImageInfo{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    write.pImageInfo = &imageInfos[index];
  }
  return OkStatus();
}

Status VulkanDevice::onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Impl& impl = *impl_;
  // The base class validated the layout reference before this hook; the layout descriptor was
  // snapshotted at layout creation, so descriptor types come from creation-time state (never a
  // by-slot lookup at encode time).
  const Impl::BindGroupLayoutRecord* layout =
      FindRecord(impl.bindGroupLayouts, descriptor.layout.slotIndex());
  if (layout == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("bind group layout slot {} has no Vulkan-side layout",
                                descriptor.layout.slotIndex())};
  }

  // One dedicated pool per bind group, sized exactly to the group's descriptor counts;
  // destroying the pool frees the set.
  const std::vector<VkDescriptorPoolSize> poolSizes = DescriptorPoolSizesFor(layout->descriptor);

  Impl::BindGroupRecord record;
  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  if (const VkResult result =
          impl_->api->vkCreateDescriptorPool(impl.device, &poolInfo, nullptr, &record.pool);
      result != VK_SUCCESS) {
    return VkError("vkCreateDescriptorPool", result);
  }

  VkDescriptorSetAllocateInfo allocateInfo = {};
  allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocateInfo.descriptorPool = record.pool;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts = &layout->layout;
  if (const VkResult result =
          impl_->api->vkAllocateDescriptorSets(impl.device, &allocateInfo, &record.set);
      result != VK_SUCCESS) {
    impl_->api->vkDestroyDescriptorPool(impl.device, record.pool, nullptr);
    return VkError("vkAllocateDescriptorSets", result);
  }

  // Write every descriptor now. Entry resources were validated live by the base class, and
  // submissions re-validate them, so a stale set can never be consumed by the GPU.
  const size_t entryCount = descriptor.entries.size();
  std::vector<VkDescriptorBufferInfo> bufferInfos(entryCount);
  std::vector<VkDescriptorImageInfo> imageInfos(entryCount);
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(entryCount);

  Status bindStatus = OkStatus();
  for (size_t i = 0; i < entryCount; ++i) {
    const BindGroupEntry& entry = descriptor.entries[i];
    VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (!FindDescriptorTypeForBinding(layout->descriptor, entry.binding, descriptorType)) {
      bindStatus =
          GpuError{GpuErrorType::InvalidState,
                   std::format("bind group entry binding {} has no layout entry", entry.binding)};
      break;
    }

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = record.set;
    write.dstBinding = entry.binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = descriptorType;

    bindStatus = impl.bindDescriptorResource(entry, i, bufferInfos, imageInfos, record, write);
    if (bindStatus.hasError()) {
      break;
    }
    writes.push_back(write);
  }
  if (bindStatus.hasError()) {
    impl_->api->vkDestroyDescriptorPool(impl.device, record.pool, nullptr);
    return bindStatus;
  }

  impl_->api->vkUpdateDescriptorSets(impl.device, static_cast<uint32_t>(writes.size()),
                                     writes.data(), 0, nullptr);
  SetSlot(impl.bindGroups, slotIndex, std::optional<Impl::BindGroupRecord>(record));
  return OkStatus();
}

Status VulkanDevice::onCreatePipelineLayout(uint32_t slotIndex,
                                            const PipelineLayoutDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Impl& impl = *impl_;
  std::vector<VkDescriptorSetLayout> setLayouts;
  setLayouts.reserve(descriptor.bindGroupLayouts.size());
  for (const BindGroupLayoutRef& layoutRef : descriptor.bindGroupLayouts) {
    const Impl::BindGroupLayoutRecord* layout =
        FindRecord(impl.bindGroupLayouts, layoutRef.slotIndex());
    if (layout == nullptr) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("bind group layout slot {} has no Vulkan-side layout",
                                  layoutRef.slotIndex())};
    }
    setLayouts.push_back(layout->layout);
  }

  VkPipelineLayoutCreateInfo layoutInfo = {};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
  layoutInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();

  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (const VkResult result =
          impl_->api->vkCreatePipelineLayout(impl.device, &layoutInfo, nullptr, &layout);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkCreatePipelineLayout for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }

  auto handle = std::make_shared<Impl::PipelineLayoutHandle>();
  handle->api = impl.api;
  handle->device = impl.device;
  handle->layout = layout;
  handle->descriptorSetCount = static_cast<uint32_t>(setLayouts.size());
  SetSlot(impl.pipelineLayouts, slotIndex, std::move(handle));
  return OkStatus();
}

Status VulkanDevice::onCreateShaderModule(uint32_t slotIndex,
                                          const ShaderModuleDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  if (descriptor.sourceKind != ShaderSourceKind::Spirv) {
    return GpuError{GpuErrorType::Unsupported, "the Vulkan backend consumes SPIR-V only"};
  }

  VkShaderModuleCreateInfo moduleInfo = {};
  moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  moduleInfo.codeSize = descriptor.spirvWords.size() * sizeof(uint32_t);
  moduleInfo.pCode = descriptor.spirvWords.data();

  VkShaderModule module = VK_NULL_HANDLE;
  if (const VkResult result =
          impl_->api->vkCreateShaderModule(impl_->device, &moduleInfo, nullptr, &module);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("vkCreateShaderModule for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }
  SetSlot(impl_->shaderModules, slotIndex, module);
  return OkStatus();
}

Status VulkanDevice::onCreateRenderPipeline(uint32_t slotIndex,
                                            const RenderPipelineDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Impl& impl = *impl_;

  const uint32_t layoutSlot = descriptor.layout.slotIndex();
  const std::shared_ptr<Impl::PipelineLayoutHandle> layout =
      layoutSlot < impl.pipelineLayouts.size() ? impl.pipelineLayouts[layoutSlot] : nullptr;
  if (layout == nullptr || layout->layout == VK_NULL_HANDLE) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("pipeline layout slot {} has no Vulkan pipeline layout", layoutSlot)};
  }

  VkShaderModule vertexModule = VK_NULL_HANDLE;
  VkShaderModule fragmentModule = VK_NULL_HANDLE;
  if (!impl.lookupShaderModules(descriptor, vertexModule, fragmentModule)) {
    return GpuError{GpuErrorType::InvalidState,
                    "render pipeline references a shader module with no Vulkan module"};
  }

  // Compatibility render pass for pipeline creation: per the specification's render pass
  // compatibility rules, load/store ops and layouts do not affect compatibility, so the
  // per-submission passes (matching formats, single sample, same attachment count) are
  // compatible with this one.
  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> colorRefs;
  BuildCompatibilityAttachments(descriptor.fragment, attachments, colorRefs);

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
  subpass.pColorAttachments = colorRefs.data();

  VkRenderPassCreateInfo renderPassInfo = {};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;

  VkRenderPass compatRenderPass = VK_NULL_HANDLE;
  if (const VkResult result =
          impl_->api->vkCreateRenderPass(impl.device, &renderPassInfo, nullptr, &compatRenderPass);
      result != VK_SUCCESS) {
    return VkError("vkCreateRenderPass (pipeline compatibility)", result);
  }

  const std::string vertexEntryPoint = descriptor.vertex.entryPoint.str();
  const std::string fragmentEntryPoint = descriptor.fragment.entryPoint.str();
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertexModule;
  stages[0].pName = vertexEntryPoint.c_str();
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragmentModule;
  stages[1].pName = fragmentEntryPoint.c_str();

  // Vertex input: vertex buffer slot N maps directly to Vulkan vertex input binding N.
  std::vector<VkVertexInputBindingDescription> vertexBindings;
  std::vector<VkVertexInputAttributeDescription> vertexAttributes;
  BuildVertexInputDescriptions(descriptor.vertex, vertexBindings, vertexAttributes);

  VkPipelineVertexInputStateCreateInfo vertexInput = {};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
  vertexInput.pVertexBindingDescriptions = vertexBindings.empty() ? nullptr : vertexBindings.data();
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
  vertexInput.pVertexAttributeDescriptions =
      vertexAttributes.empty() ? nullptr : vertexAttributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = ToVkTopology(descriptor.topology);
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  // Viewport and scissor are dynamic; counts are still required.
  VkPipelineViewportStateCreateInfo viewportState = {};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  // Front face is counter-clockwise: with the negative-viewport-height flip the framebuffer
  // coordinate mapping matches WebGPU exactly, so WebGPU's CCW default carries over unchanged.
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.depthClampEnable = VK_FALSE;
  rasterization.rasterizerDiscardEnable = VK_FALSE;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = ToVkCullMode(descriptor.cullMode);
  rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization.depthBiasEnable = VK_FALSE;
  rasterization.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  const std::vector<VkPipelineColorBlendAttachmentState> blendAttachments =
      BuildBlendAttachments(descriptor.fragment);

  VkPipelineColorBlendStateCreateInfo colorBlend = {};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.logicOpEnable = VK_FALSE;
  colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
  colorBlend.pAttachments = blendAttachments.data();

  const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState = {};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamicStates;

  VkGraphicsPipelineCreateInfo pipelineInfo = {};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterization;
  pipelineInfo.pMultisampleState = &multisample;
  pipelineInfo.pColorBlendState = &colorBlend;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = layout->layout;
  pipelineInfo.renderPass = compatRenderPass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  pipelineInfo.basePipelineIndex = -1;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (const VkResult result = impl_->api->vkCreateGraphicsPipelines(
          impl.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
      result != VK_SUCCESS) {
    impl_->api->vkDestroyRenderPass(impl.device, compatRenderPass, nullptr);
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("vkCreateGraphicsPipelines for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }

  SetSlot(impl.renderPipelines, slotIndex,
          std::optional<Impl::RenderPipelineRecord>(
              Impl::RenderPipelineRecord{pipeline, compatRenderPass, layout}));
  return OkStatus();
}

void VulkanDevice::Impl::destroyBufferSlot(uint32_t slotIndex) {
  if (BufferRecord* record = FindRecord(buffers, slotIndex)) {
    if (record->uploadSerial > completedSerialValue) {
      const auto submission =
          std::ranges::find_if(inFlight, [&](const InFlightSubmission& pending) {
            return pending.serial == record->uploadSerial;
          });
      UTILS_RELEASE_ASSERT(submission != inFlight.end());
      submission->retiredBuffers.push_back(*record);
    } else {
      destroyBufferRecord(*record);
    }
    buffers[slotIndex].reset();
  }
}

void VulkanDevice::Impl::destroyTextureSlot(uint32_t slotIndex) {
  if (TextureRecord* record = FindRecord(textures, slotIndex)) {
    // The latest queued upload covers earlier copies on this in-order queue.
    auto pending = std::find_if(pendingUploads.rbegin(), pendingUploads.rend(),
                                [slotIndex](const PendingUpload& upload) {
                                  return upload.textureSlot == slotIndex && !upload.retiredTexture;
                                });
    if (pending != pendingUploads.rend()) {
      pending->retiredTexture = *record;
    } else {
      destroyTextureRecord(*record);
    }
    textures[slotIndex].reset();
    // The slot is recyclable now, and a new image in it has touched nothing; leaving this
    // texture's state behind would have the next one's first barrier wait on work that ran
    // against an image that no longer exists.
    syncStates.forget(slotIndex);
  }
}

void VulkanDevice::Impl::destroyTextureViewSlot(uint32_t slotIndex) {
  if (TextureViewRecord* record = FindRecord(textureViews, slotIndex)) {
    DestroyIfSet(device, record->view, api->vkDestroyImageView);
    textureViews[slotIndex].reset();
  }
}

void VulkanDevice::Impl::destroySamplerSlot(uint32_t slotIndex) {
  if (slotIndex < samplers.size()) {
    DestroyIfSet(device, samplers[slotIndex], api->vkDestroySampler);
  }
}

void VulkanDevice::Impl::destroyBindGroupLayoutSlot(uint32_t slotIndex) {
  if (BindGroupLayoutRecord* record = FindRecord(bindGroupLayouts, slotIndex)) {
    DestroyIfSet(device, record->layout, api->vkDestroyDescriptorSetLayout);
    bindGroupLayouts[slotIndex].reset();
  }
}

void VulkanDevice::Impl::destroyBindGroupSlot(uint32_t slotIndex) {
  if (BindGroupRecord* record = FindRecord(bindGroups, slotIndex)) {
    DestroyIfSet(device, record->pool, api->vkDestroyDescriptorPool);
    bindGroups[slotIndex].reset();
  }
}

void VulkanDevice::Impl::destroyPipelineLayoutSlot(uint32_t slotIndex) {
  if (slotIndex < pipelineLayouts.size()) {
    // Pipelines retain the layout through the shared handle; dropping the slot's reference
    // destroys the VkPipelineLayout once the last pipeline using it is destroyed.
    pipelineLayouts[slotIndex].reset();
  }
}

void VulkanDevice::Impl::destroyShaderModuleSlot(uint32_t slotIndex) {
  if (slotIndex < shaderModules.size()) {
    // Per the specification, a shader module may be destroyed while pipelines created from it
    // are still in use.
    DestroyIfSet(device, shaderModules[slotIndex], api->vkDestroyShaderModule);
  }
}

void VulkanDevice::Impl::destroyRenderPipelineSlot(uint32_t slotIndex) {
  if (RenderPipelineRecord* record = FindRecord(renderPipelines, slotIndex)) {
    DestroyIfSet(device, record->pipeline, api->vkDestroyPipeline);
    DestroyIfSet(device, record->compatRenderPass, api->vkDestroyRenderPass);
    renderPipelines[slotIndex].reset();  // Releases the retained pipeline layout.
  }
}

Status VulkanDevice::onCreateComputePipeline(uint32_t slotIndex,
                                             const ComputePipelineDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Impl& impl = *impl_;

  const uint32_t layoutSlot = descriptor.layout.slotIndex();
  const std::shared_ptr<Impl::PipelineLayoutHandle> layout =
      layoutSlot < impl.pipelineLayouts.size() ? impl.pipelineLayouts[layoutSlot] : nullptr;
  if (layout == nullptr || layout->layout == VK_NULL_HANDLE) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("pipeline layout slot {} has no Vulkan pipeline layout", layoutSlot)};
  }
  const uint32_t moduleSlot = descriptor.compute.module.slotIndex();
  const VkShaderModule module =
      moduleSlot < impl.shaderModules.size() ? impl.shaderModules[moduleSlot] : VK_NULL_HANDLE;
  if (module == VK_NULL_HANDLE) {
    return GpuError{GpuErrorType::InvalidState,
                    "compute pipeline references a shader module with no Vulkan module"};
  }

  const std::string entryPoint(std::string_view(descriptor.compute.entryPoint));
  VkPipelineShaderStageCreateInfo stage = {};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = module;
  stage.pName = entryPoint.c_str();

  VkComputePipelineCreateInfo pipelineInfo = {};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = stage;
  pipelineInfo.layout = layout->layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (const VkResult result = impl.api->vkCreateComputePipelines(impl.device, VK_NULL_HANDLE, 1,
                                                                 &pipelineInfo, nullptr, &pipeline);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("vkCreateComputePipelines for '{}' failed with {}",
                                std::string_view(descriptor.label), VkResultToString(result))};
  }

  SetSlot(
      impl.computePipelines, slotIndex,
      std::optional<Impl::ComputePipelineRecord>(Impl::ComputePipelineRecord{pipeline, layout}));
  return OkStatus();
}

void VulkanDevice::Impl::destroyComputePipelineSlot(uint32_t slotIndex) {
  if (Impl::ComputePipelineRecord* record = FindRecord(computePipelines, slotIndex)) {
    if (record->pipeline != VK_NULL_HANDLE) {
      api->vkDestroyPipeline(device, record->pipeline, nullptr);
    }
    computePipelines[slotIndex].reset();  // Releases the retained pipeline layout.
  }
}

void VulkanDevice::onRetireBuffer(uint32_t slotIndex) {
  impl_->discardPendingBufferWrites(slotIndex);
  if (impl_->mappingTable) {
    impl_->mappingTable->invalidateBuffer(slotIndex);
  }
}

void VulkanDevice::Impl::destroyResourceSlot(std::string_view resourceName, uint32_t slotIndex) {
  // Buffer slots also retain destinations used only by queued uploads, outside the public stream.
  if (resourceName == "buffer") {
    destroyBufferSlot(slotIndex);
  } else if (resourceName == "texture") {
    destroyTextureSlot(slotIndex);
  } else if (resourceName == "textureView") {
    destroyTextureViewSlot(slotIndex);
  } else if (resourceName == "sampler") {
    destroySamplerSlot(slotIndex);
  } else if (resourceName == "bindGroupLayout") {
    destroyBindGroupLayoutSlot(slotIndex);
  } else if (resourceName == "bindGroup") {
    destroyBindGroupSlot(slotIndex);
  } else if (resourceName == "pipelineLayout") {
    destroyPipelineLayoutSlot(slotIndex);
  } else if (resourceName == "shaderModule") {
    destroyShaderModuleSlot(slotIndex);
  } else if (resourceName == "renderPipeline") {
    destroyRenderPipelineSlot(slotIndex);
  } else if (resourceName == "computePipeline") {
    destroyComputePipelineSlot(slotIndex);
  }
}

void VulkanDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  if (impl_->executionUncertain || impl_->surfaceLifetimeUnproven()) {
    return;
  }
  Impl& impl = *impl_;
  impl.destroyResourceSlot(resourceName, slotIndex);
}

Status VulkanDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                   std::span<const uint8_t> data) {
  const Impl::BufferRecord* record = FindRecord(impl_->buffers, slotIndex);
  if (record == nullptr || record->allocation.mapped == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no Vulkan buffer", slotIndex)};
  }
  if (data.empty()) {
    return OkStatus();
  }
  const uint64_t lastUse = std::max(bufferLastUseSerial(slotIndex), record->uploadSerial);
  const uint64_t completed = completedSerial();
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, lastErrorForTest()};
  }
  if (lastUse > completed || impl_->hasPendingBufferWrite(slotIndex)) {
    if (offsetBytes % 4 == 0 && data.size() % 4 == 0) {
      return impl_->queueBufferWrite(slotIndex, offsetBytes, data);
    }
    if (Status status = waitForBufferAccess(lastUse, "writeBuffer"); status.hasError()) {
      return status;
    }
    impl_->flushPendingBufferWritesToHost(slotIndex, record->allocation.mapped);
  }
  std::memcpy(static_cast<uint8_t*>(record->allocation.mapped) + offsetBytes, data.data(),
              data.size());
  return OkStatus();
}

Status VulkanDevice::Impl::encodePendingBufferWrites(EncodingState& state) {
  if (pendingBufferWrites.empty()) {
    return OkStatus();
  }
  auto staging = createHostVisibleBuffer(pendingBufferWriteBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         "queued buffer writes");
  if (staging.hasError()) {
    return std::move(staging).error();
  }
  state.bufferWriteStaging = std::move(staging).result();
  ++bufferWriteStagingAllocations;

  VkMemoryBarrier hostBarrier = {};
  hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  hostBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  api->vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hostBarrier, 0, nullptr, 0,
                            nullptr);
  VkDeviceSize stagingOffset = 0;
  for (const PendingBufferWrite& write : pendingBufferWrites) {
    const BufferRecord* destination = FindRecord(buffers, write.slotIndex);
    if (destination == nullptr) {
      return GpuError{GpuErrorType::InvalidState, "queued buffer write lost its destination"};
    }
    std::memcpy(static_cast<uint8_t*>(state.bufferWriteStaging.allocation.mapped) + stagingOffset,
                write.bytes.data(), write.bytes.size());
    VkBufferMemoryBarrier beforeCopy = {};
    beforeCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    beforeCopy.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    beforeCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    beforeCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beforeCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beforeCopy.buffer = destination->buffer;
    beforeCopy.offset = write.offsetBytes;
    beforeCopy.size = write.bytes.size();
    api->vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &beforeCopy, 0,
                              nullptr);
    const VkBufferCopy copy{stagingOffset, write.offsetBytes, write.bytes.size()};
    api->vkCmdCopyBuffer(state.commandBuffer, state.bufferWriteStaging.buffer, destination->buffer,
                         1, &copy);
    stagingOffset += write.bytes.size();
  }
  VkMemoryBarrier afterCopies = {};
  afterCopies.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  afterCopies.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  afterCopies.dstAccessMask =
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
  api->vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                            &afterCopies, 0, nullptr, 0, nullptr);
  return OkStatus();
}

void VulkanDevice::Impl::destroyUploadObjects(VkFence fence, VkCommandBuffer commandBuffer,
                                              BufferRecord& staging) {
  if (fence != VK_NULL_HANDLE) {
    api->vkDestroyFence(device, fence, nullptr);
  }
  if (commandBuffer != VK_NULL_HANDLE) {
    api->vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
  }
  destroyBufferRecord(staging);
}

void VulkanDevice::Impl::recordTextureUploadCopy(VkCommandBuffer commandBuffer,
                                                 uint32_t textureSlot, const TextureRecord& texture,
                                                 VkBuffer stagingBuffer,
                                                 const TexelCopyBufferLayout& dataLayout,
                                                 const Extent2d& writeSize,
                                                 const Origin2d& destinationOrigin) {
  transitionTexture(commandBuffer, textureSlot, texture, TextureUsageKind::TransferWrite);

  const uint32_t texelSize = TextureFormatBytesPerTexel(texture.format);
  VkBufferImageCopy copyRegion = {};
  copyRegion.bufferOffset = dataLayout.offsetBytes;
  copyRegion.bufferRowLength = dataLayout.bytesPerRow / texelSize;  // In texels.
  copyRegion.bufferImageHeight = dataLayout.rowsPerImage;
  copyRegion.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.imageOffset = VkOffset3D{static_cast<int32_t>(destinationOrigin.x),
                                      static_cast<int32_t>(destinationOrigin.y), 0};
  copyRegion.imageExtent = VkExtent3D{writeSize.width, writeSize.height, 1};
  api->vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, texture.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

  // Sampled textures move straight to their descriptor layout; others stay transfer-dst until a
  // later encode transitions them.
  if (HasAllFlags(texture.usage, TextureUsage::Sampled)) {
    transitionTexture(commandBuffer, textureSlot, texture, TextureUsageKind::SampledRead);
  }
}

Status VulkanDevice::Impl::submitAndWaitTextureUpload(VkCommandBuffer commandBuffer, VkFence& fence,
                                                      bool& objectsStillInUse, bool& reachedQueue) {
  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (const VkResult result = api->vkCreateFence(device, &fenceInfo, nullptr, &fence);
      result != VK_SUCCESS) {
    return VkError("vkCreateFence", result);
  }

  if (const VkResult result = submitToQueue({&commandBuffer, 1}, fence); result != VK_SUCCESS) {
    if (!SubmissionWasRejected(result) &&
        !drainAfterDeviceLoss(result, "vkQueueSubmit (writeTexture)")) {
      objectsStillInUse = true;
      executionUncertain = true;
      recordError(std::format("vkQueueSubmit (writeTexture) completion unknown: {}",
                              VkResultToString(result)));
    }
    return VkError("vkQueueSubmit (writeTexture)", result);
  }
  // Past this point the work is the queue's, and it will run whatever this call reports.
  reachedQueue = true;

  if (uploadFailureMode == UploadFailureModeForTest::AfterSubmit) {
    // Test seam: stand in for a wait that times out on a submission the queue has accepted.
    uploadFailureMode.reset();
    objectsStillInUse = true;
    return GpuError{GpuErrorType::InvalidState,
                    "writeTexture: injected post-submit upload timeout"};
  }

  return waitForTextureUpload(fence, objectsStillInUse);
}

Status VulkanDevice::Impl::waitForTextureUpload(VkFence fence, bool& objectsStillInUse) {
  const FenceWait waited = waitForFenceUnlessLost(fence, kUploadFenceTimeoutSeconds);
  if (waited.end == FenceWaitEnd::Signalled) {
    return OkStatus();
  }
  if (waited.end == FenceWaitEnd::Failed && waited.result == VK_ERROR_DEVICE_LOST) {
    recordDeviceLoss("vkWaitForFences (writeTexture) reported device loss");
    return VkError("vkWaitForFences (writeTexture)", waited.result);
  }
  // A wait that did not see the fence signal does not prove completion, whatever ended it, so
  // ownership is retained until polling or teardown does.
  objectsStillInUse = true;
  if (waited.end == FenceWaitEnd::RootLost) {
    // A loss another device over the root declared leaves no error here; it is still a loss.
    if (!hasError()) {
      return GpuError{GpuErrorType::DeviceLost,
                      "writeTexture cannot wait for its upload: the device was lost"};
    }
    return GpuError{GpuErrorType::InvalidState, errorMessage()};
  }
  return VkError("vkWaitForFences (writeTexture, still pending)", waited.result);
}

Status VulkanDevice::Impl::finishTextureUpload(uint32_t slotIndex, VkCommandBuffer commandBuffer,
                                               VkFence& fence, BufferRecord& staging,
                                               bool& reachedQueue) {
  const auto cleanup = [&] { destroyUploadObjects(fence, commandBuffer, staging); };
  bool objectsStillInUse = false;
  const Status submitStatus =
      submitAndWaitTextureUpload(commandBuffer, fence, objectsStillInUse, reachedQueue);
  if (submitStatus.hasError()) {
    if (objectsStillInUse) {
      pendingUploads.push_back({fence, commandBuffer, staging, slotIndex, std::nullopt});
    } else {
      cleanup();
    }
    return submitStatus;
  }

  cleanup();
  return OkStatus();
}

Status VulkanDevice::onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                    const TexelCopyBufferLayout& dataLayout,
                                    const Extent2d& writeSize, const Origin2d& destinationOrigin) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  Impl& impl = *impl_;
  Impl::TextureRecord* texture = FindRecord(impl.textures, slotIndex);
  if (texture == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no Vulkan image", slotIndex)};
  }

  // Same conservative alignment rule as copyTextureToBuffer (see onSubmit): keep every
  // buffer-image copy offset 4-byte aligned so uploads stay portable across queue/format
  // combinations. Only sub-4-byte R8 offsets can trip this.
  if (dataLayout.offsetBytes % 4 != 0) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("writeTexture: offsetBytes {} is not 4-byte aligned; the Vulkan "
                                "backend requires 4-byte-aligned copy offsets",
                                dataLayout.offsetBytes)};
  }

  // Staged upload through a transient host-visible buffer, executed synchronously: the internal
  // submission is fenced and waited on before returning, which both orders the write against
  // every previously submitted use of the image (single in-order queue) and lets the staging
  // buffer be destroyed immediately.
  Result<Impl::BufferRecord> stagingResult = impl.createHostVisibleBuffer(
      data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "writeTexture staging");
  if (stagingResult.hasError()) {
    return std::move(stagingResult).error();
  }
  Impl::BufferRecord staging = std::move(stagingResult).result();
  std::memcpy(staging.allocation.mapped, data.data(), data.size());

  // Fail-closed cleanup for every early return below.
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  const auto cleanup = [&]() { impl.destroyUploadObjects(fence, commandBuffer, staging); };

  Result<VkCommandBuffer> commandBufferResult = impl.allocateCommandBuffer();
  if (commandBufferResult.hasError()) {
    cleanup();
    return std::move(commandBufferResult).error();
  }
  commandBuffer = commandBufferResult.result();

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (const VkResult result = impl_->api->vkBeginCommandBuffer(commandBuffer, &beginInfo);
      result != VK_SUCCESS) {
    cleanup();
    return VkError("vkBeginCommandBuffer", result);
  }

  // Accepted submissions retain their transitions even if the wait fails. Unsubmitted work is
  // discarded. Device loss makes layouts undefined and blocks later use through the error latch.
  struct StagedUploadGuard {
    Impl* impl = nullptr;       //!< Device whose staged state this guards.
    bool reachedQueue = false;  //!< Whether the queue accepted the submission.

    ~StagedUploadGuard() {
      if (reachedQueue) {
        impl->syncStates.commitStaged();
      } else {
        impl->syncStates.discardStaged();
      }
    }
  } stagedUploadGuard{&impl, false};

  impl.recordTextureUploadCopy(commandBuffer, slotIndex, *texture, staging.buffer, dataLayout,
                               writeSize, destinationOrigin);

  if (const VkResult result = impl_->api->vkEndCommandBuffer(commandBuffer); result != VK_SUCCESS) {
    cleanup();
    return VkError("vkEndCommandBuffer", result);
  }

  if (impl.uploadFailureMode == UploadFailureModeForTest::BeforeSubmit) {
    // Test seam: fail exactly where a real end-of-recording or submit failure would, which is
    // after the transitions have been staged and before anything reaches the queue.
    impl.uploadFailureMode.reset();
    cleanup();
    return GpuError{GpuErrorType::InvalidState, "writeTexture: injected pre-submit failure"};
  }

  return impl.finishTextureUpload(slotIndex, commandBuffer, fence, staging,
                                  stagedUploadGuard.reachedQueue);
}

void VulkanDevice::Impl::transitionTexture(VkCommandBuffer commandBuffer, uint32_t textureSlot,
                                           const TextureRecord& record, TextureUsageKind usage) {
  // Before the early return below: a use that needs no barrier is still a use, and a submission
  // that writes a surface's frame has to carry that frame's acquisition wait either way.
  encodedTextureSlots.push_back(textureSlot);

  const TextureSyncState current = syncStates.stateOf(textureSlot);
  const ImageBarrierParams params = TransitionFor(current, usage);
  const bool wasWritten = current.access != 0 && params.srcAccess != 0;
  if (params.oldLayout == params.newLayout && !wasWritten) {
    return;  // Same layout and nothing to make available: no hazard to order.
  }
  RecordImageBarrier(*api, commandBuffer, record.image, params);
  if (recordedBarriers) {
    recordedBarriers->push_back(RecordedImageBarrierForTest{
        textureSlot, static_cast<uint32_t>(params.srcStage), static_cast<uint32_t>(params.dstStage),
        static_cast<uint32_t>(params.srcAccess), static_cast<uint32_t>(params.dstAccess),
        static_cast<int32_t>(params.oldLayout), static_cast<int32_t>(params.newLayout)});
  }
  syncStates.stage(textureSlot, StateAfterUsage(usage));
}

void VulkanDevice::Impl::destroyTransientEncodingObjects(EncodingState& state) {
  for (VkFramebuffer framebuffer : state.transientFramebuffers) {
    api->vkDestroyFramebuffer(device, framebuffer, nullptr);
  }
  for (VkRenderPass renderPass : state.transientRenderPasses) {
    api->vkDestroyRenderPass(device, renderPass, nullptr);
  }
  // Freeing a command buffer in the recording state is legal; it has not been submitted.
  if (!state.commandBuffers.empty()) {
    api->vkFreeCommandBuffers(device, commandPool,
                              static_cast<uint32_t>(state.commandBuffers.size()),
                              state.commandBuffers.data());
  }
}

void VulkanDevice::Impl::transitionBoundTextures(EncodingState& state,
                                                 const BindGroupRecord& group) {
  const auto transitionTo = [&](uint32_t textureSlot, TextureUsageKind usage) {
    if (const TextureRecord* texture = FindRecord(textures, textureSlot)) {
      transitionTexture(state.commandBuffer, textureSlot, *texture, usage);
    }
  };
  for (const uint32_t sampledSlot : group.sampledTextureSlots) {
    transitionTo(sampledSlot, TextureUsageKind::SampledRead);
  }
  for (const uint32_t storageSlot : group.storageTextureSlots) {
    transitionTo(storageSlot, TextureUsageKind::StorageWrite);
  }
}

void VulkanDevice::Impl::transitionRenderPassBoundTextures(EncodingState& state,
                                                           std::span<const Command> commands,
                                                           size_t beginIndex) {
  for (size_t scanIndex = beginIndex + 1; scanIndex < commands.size(); ++scanIndex) {
    if (std::get_if<EndRenderPassCommand>(&commands[scanIndex]) != nullptr) {
      break;
    }
    const auto* scannedBindGroup = std::get_if<SetBindGroupCommand>(&commands[scanIndex]);
    if (scannedBindGroup == nullptr) {
      continue;
    }
    const BindGroupRecord* scannedGroup =
        FindRecord(bindGroups, scannedBindGroup->bindGroupId.slotIndex);
    if (scannedGroup == nullptr) {
      continue;  // The SetBindGroupCommand handler below fails closed on this.
    }
    transitionBoundTextures(state, *scannedGroup);
  }
}

Status VulkanDevice::Impl::beginEncodedRenderPass(EncodingState& state,
                                                  const BeginRenderPassCommand& beginPass) {
  const std::vector<RenderPassColorAttachment>& attachmentDescriptors =
      beginPass.descriptor.colorAttachments;

  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> colorRefs;
  std::vector<VkImageView> attachmentViews;
  std::vector<VkClearValue> clearValues;

  for (size_t i = 0; i < attachmentDescriptors.size(); ++i) {
    const RenderPassColorAttachment& attachment = attachmentDescriptors[i];
    const TextureViewRecord* view = FindRecord(textureViews, attachment.view.slotIndex());
    TextureRecord* texture = view != nullptr ? FindRecord(textures, view->textureSlot) : nullptr;
    if (view == nullptr || texture == nullptr) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("render pass attachment {} does not resolve to a Vulkan image", i)};
    }

    // Explicit transition to the attachment layout; the pass then begins and ends in
    // COLOR_ATTACHMENT_OPTIMAL, so the pass itself performs no layout transition.
    transitionTexture(state.commandBuffer, view->textureSlot, *texture,
                      TextureUsageKind::ColorAttachment);

    VkAttachmentDescription attachmentDescription = {};
    attachmentDescription.format = ToVkFormat(texture->format);
    attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
    attachmentDescription.loadOp = ToVkLoadOp(attachment.loadOp);
    attachmentDescription.storeOp = ToVkStoreOp(attachment.storeOp);
    attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments.push_back(attachmentDescription);
    colorRefs.push_back(
        VkAttachmentReference{static_cast<uint32_t>(i), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    attachmentViews.push_back(view->view);

    VkClearValue clearValue = {};
    clearValue.color.float32[0] = static_cast<float>(attachment.clearColor[0]);
    clearValue.color.float32[1] = static_cast<float>(attachment.clearColor[1]);
    clearValue.color.float32[2] = static_cast<float>(attachment.clearColor[2]);
    clearValue.color.float32[3] = static_cast<float>(attachment.clearColor[3]);
    clearValues.push_back(clearValue);

    // All attachments share one extent (base-class beginRenderPass validation), so the
    // last one is authoritative.
    state.passExtent = texture->size;
  }

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
  subpass.pColorAttachments = colorRefs.data();

  // Per-image barriers provide synchronization without changing pipeline render-pass compatibility.
  VkRenderPassCreateInfo renderPassInfo = {};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  if (const VkResult result =
          api->vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass);
      result != VK_SUCCESS) {
    return VkError("vkCreateRenderPass", result);
  }
  state.transientRenderPasses.push_back(renderPass);

  VkFramebufferCreateInfo framebufferInfo = {};
  framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebufferInfo.renderPass = renderPass;
  framebufferInfo.attachmentCount = static_cast<uint32_t>(attachmentViews.size());
  framebufferInfo.pAttachments = attachmentViews.data();
  framebufferInfo.width = state.passExtent.width;
  framebufferInfo.height = state.passExtent.height;
  framebufferInfo.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (const VkResult result =
          api->vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer);
      result != VK_SUCCESS) {
    return VkError("vkCreateFramebuffer", result);
  }
  state.transientFramebuffers.push_back(framebuffer);

  VkRenderPassBeginInfo passBeginInfo = {};
  passBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  passBeginInfo.renderPass = renderPass;
  passBeginInfo.framebuffer = framebuffer;
  passBeginInfo.renderArea = VkRect2D{{0, 0}, {state.passExtent.width, state.passExtent.height}};
  passBeginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
  passBeginInfo.pClearValues = clearValues.data();
  api->vkCmdBeginRenderPass(state.commandBuffer, &passBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
  state.inRenderPass = true;

  // WebGPU-style pass defaults: full-attachment viewport and scissor. The negative-height
  // viewport (VK_KHR_maintenance1, core in 1.1) flips Vulkan's y-down clip space to match
  // the WebGPU/Metal convention the shared shaders and MVPs assume.
  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = static_cast<float>(state.passExtent.height);
  viewport.width = static_cast<float>(state.passExtent.width);
  viewport.height = -static_cast<float>(state.passExtent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  api->vkCmdSetViewport(state.commandBuffer, 0, 1, &viewport);
  const VkRect2D scissor = {{0, 0}, {state.passExtent.width, state.passExtent.height}};
  api->vkCmdSetScissor(state.commandBuffer, 0, 1, &scissor);

  // Fresh pass state, matching WebGPU render pass semantics.
  state.currentPipeline = nullptr;
  state.boundGroups.fill(nullptr);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetPipeline(EncodingState& state,
                                             const SetPipelineCommand& setPipeline) {
  const RenderPipelineRecord* pipeline =
      FindRecord(renderPipelines, setPipeline.pipelineId.slotIndex);
  if (!state.inRenderPass || pipeline == nullptr || pipeline->pipeline == VK_NULL_HANDLE) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  api->vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
  state.currentPipeline = pipeline;
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetBindGroup(EncodingState& state,
                                              const SetBindGroupCommand& setBindGroup) {
  const BindGroupRecord* bindGroup = FindRecord(bindGroups, setBindGroup.bindGroupId.slotIndex);
  if ((!state.inRenderPass && !state.inComputePass) || bindGroup == nullptr ||
      setBindGroup.index >= kMaxBindGroups) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setBindGroup: bind group slot {} is not encodable",
                                setBindGroup.bindGroupId.slotIndex)};
  }
  // Descriptor sets bind lazily at draw: vkCmdBindDescriptorSets needs the pipeline layout,
  // and the RHI allows setBindGroup before setPipeline.
  state.boundGroups[setBindGroup.index] = bindGroup;
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetVertexBuffer(EncodingState& state,
                                                 const SetVertexBufferCommand& setVertexBuffer) {
  const BufferRecord* buffer = FindRecord(buffers, setVertexBuffer.bufferId.slotIndex);
  if (!state.inRenderPass || buffer == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setVertexBuffer: buffer slot {} is not encodable",
                                setVertexBuffer.bufferId.slotIndex)};
  }
  const VkDeviceSize offset = setVertexBuffer.offsetBytes;
  api->vkCmdBindVertexBuffers(state.commandBuffer, setVertexBuffer.slot, 1, &buffer->buffer,
                              &offset);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetIndexBuffer(EncodingState& state,
                                                const SetIndexBufferCommand& setIndexBuffer) {
  const BufferRecord* buffer = FindRecord(buffers, setIndexBuffer.bufferId.slotIndex);
  if (!state.inRenderPass || buffer == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setIndexBuffer: buffer slot {} is not encodable",
                                setIndexBuffer.bufferId.slotIndex)};
  }
  // Deferred rather than bound here: an empty binding at the buffer end is valid for the encoder
  // but an invalid vkCmdBindIndexBuffer offset, and a zero-count draw never needs it.
  state.pendingIndexBinding = EncodingState::PendingIndexBinding{
      buffer->buffer, setIndexBuffer.offsetBytes,
      setIndexBuffer.format == IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32};
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetScissorRect(EncodingState& state,
                                                const SetScissorRectCommand& setScissor) {
  if (!state.inRenderPass) {
    return GpuError{GpuErrorType::InvalidState, "setScissorRect outside a render pass"};
  }
  const VkRect2D scissor = {
      {static_cast<int32_t>(setScissor.x), static_cast<int32_t>(setScissor.y)},
      {setScissor.width, setScissor.height}};
  api->vkCmdSetScissor(state.commandBuffer, 0, 1, &scissor);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetViewport(EncodingState& state,
                                             const SetViewportCommand& setViewport) {
  if (!state.inRenderPass) {
    return GpuError{GpuErrorType::InvalidState, "setViewport outside a render pass"};
  }
  // Same y-flip as the pass default so explicit viewports keep WebGPU semantics.
  VkViewport viewport = {};
  viewport.x = setViewport.x;
  viewport.y = setViewport.y + setViewport.height;
  viewport.width = setViewport.width;
  viewport.height = -setViewport.height;
  viewport.minDepth = setViewport.minDepth;
  viewport.maxDepth = setViewport.maxDepth;
  api->vkCmdSetViewport(state.commandBuffer, 0, 1, &viewport);
  return OkStatus();
}

Status VulkanDevice::Impl::bindDrawDescriptorSets(EncodingState& state,
                                                  std::string_view operation) {
  if (!state.inRenderPass || state.currentPipeline == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("{} without an active pass and pipeline", operation)};
  }
  // Bind every set the pipeline layout declares. The encoder's draw-time validation
  // guarantees each declared group index is bound; this re-check fails closed anyway.
  for (uint32_t setIndex = 0; setIndex < state.currentPipeline->layout->descriptorSetCount;
       ++setIndex) {
    const BindGroupRecord* group = state.boundGroups[setIndex];
    if (group == nullptr) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("{}: pipeline layout requires bind group {} but none is bound",
                                  operation, setIndex)};
    }
    api->vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 state.currentPipeline->layout->layout, setIndex, 1, &group->set, 0,
                                 nullptr);
  }
  return OkStatus();
}

Status VulkanDevice::Impl::encodeDraw(EncodingState& state, const DrawCommand& draw) {
  if (Status status = bindDrawDescriptorSets(state, "draw"); status.hasError()) {
    return status;
  }
  api->vkCmdDraw(state.commandBuffer, draw.vertexCount, draw.instanceCount, draw.firstVertex,
                 draw.firstInstance);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeDrawIndexed(EncodingState& state, const DrawIndexedCommand& draw) {
  if (Status status = bindDrawDescriptorSets(state, "drawIndexed"); status.hasError()) {
    return status;
  }
  if (IsEmptyIndexedDraw(draw)) {
    return OkStatus();
  }
  // The encoder bounded this draw's index range inside the binding, so the offset is in range.
  if (state.pendingIndexBinding) {
    api->vkCmdBindIndexBuffer(state.commandBuffer, state.pendingIndexBinding->buffer,
                              state.pendingIndexBinding->offsetBytes,
                              state.pendingIndexBinding->indexType);
    state.pendingIndexBinding.reset();
  }
  api->vkCmdDrawIndexed(state.commandBuffer, draw.indexCount, draw.instanceCount, draw.firstIndex,
                        draw.baseVertex, draw.firstInstance);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeEndRenderPass(EncodingState& state) {
  if (!state.inRenderPass) {
    return GpuError{GpuErrorType::InvalidState, "endRenderPass without an active render pass"};
  }
  api->vkCmdEndRenderPass(state.commandBuffer);
  state.inRenderPass = false;
  state.currentPipeline = nullptr;
  state.boundGroups.fill(nullptr);
  state.pendingIndexBinding.reset();
  return OkStatus();
}

Status VulkanDevice::Impl::encodeCopyTextureToBuffer(EncodingState& state,
                                                     const CopyTextureToBufferCommand& copy) {
  if (state.inRenderPass) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToBuffer inside a render pass"};
  }
  TextureRecord* texture = FindRecord(textures, copy.textureId.slotIndex);
  const BufferRecord* buffer = FindRecord(buffers, copy.bufferId.slotIndex);
  if (texture == nullptr || buffer == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToBuffer: source texture or destination buffer is missing"};
  }
  // The shared validation enforces texel-size alignment; enforce 4-byte alignment on top,
  // uniformly. Vulkan's buffer-image copy rules ("Copies to and from Buffer Memory")
  // additionally require 4-byte-aligned bufferOffset for some queue/format combinations, so
  // rejecting the rare sub-4-byte offset (possible only for R8) keeps every copy portable.
  if (copy.layout.offsetBytes % 4 != 0) {
    return GpuError{
        GpuErrorType::Unsupported,
        std::format("copyTextureToBuffer: offsetBytes {} is not 4-byte aligned; the Vulkan "
                    "backend requires 4-byte-aligned copy offsets",
                    copy.layout.offsetBytes)};
  }

  transitionTexture(state.commandBuffer, copy.textureId.slotIndex, *texture,
                    TextureUsageKind::TransferRead);

  const uint32_t texelSize = TextureFormatBytesPerTexel(texture->format);
  VkBufferImageCopy copyRegion = {};
  copyRegion.bufferOffset = copy.layout.offsetBytes;
  copyRegion.bufferRowLength = copy.layout.bytesPerRow / texelSize;  // In texels.
  copyRegion.bufferImageHeight = copy.layout.rowsPerImage;
  copyRegion.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.imageOffset = VkOffset3D{0, 0, 0};
  copyRegion.imageExtent = VkExtent3D{copy.copySize.width, copy.copySize.height, 1};
  api->vkCmdCopyImageToBuffer(state.commandBuffer, texture->image,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &copyRegion);

  // A CopyDst buffer may next be consumed through any other declared BufferUsage in this stream.
  // Preserve host readback while making the write visible to draw, shader, and transfer commands.
  VkBufferMemoryBarrier bufferBarrier = {};
  bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                VK_ACCESS_TRANSFER_WRITE_BIT;
  bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufferBarrier.buffer = buffer->buffer;
  bufferBarrier.offset = 0;
  bufferBarrier.size = VK_WHOLE_SIZE;
  constexpr VkPipelineStageFlags kBufferConsumerStages =
      VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  api->vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            kBufferConsumerStages, 0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);
  return OkStatus();
}

std::optional<Status> VulkanDevice::Impl::encodeRenderCommand(EncodingState& state,
                                                              std::span<const Command> commands,
                                                              size_t commandIndex) {
  const Command& command = commands[commandIndex];
  if (const auto* beginPass = std::get_if<BeginRenderPassCommand>(&command)) {
    transitionRenderPassBoundTextures(state, commands, commandIndex);
    return beginEncodedRenderPass(state, *beginPass);
  } else if (const auto* setPipeline = std::get_if<SetPipelineCommand>(&command)) {
    return encodeSetPipeline(state, *setPipeline);
  } else if (const auto* setBindGroup = std::get_if<SetBindGroupCommand>(&command)) {
    return encodeSetBindGroup(state, *setBindGroup);
  } else if (const auto* setVertexBuffer = std::get_if<SetVertexBufferCommand>(&command)) {
    return encodeSetVertexBuffer(state, *setVertexBuffer);
  } else if (const auto* setIndexBuffer = std::get_if<SetIndexBufferCommand>(&command)) {
    return encodeSetIndexBuffer(state, *setIndexBuffer);
  } else if (const auto* setScissor = std::get_if<SetScissorRectCommand>(&command)) {
    return encodeSetScissorRect(state, *setScissor);
  } else if (const auto* setViewport = std::get_if<SetViewportCommand>(&command)) {
    return encodeSetViewport(state, *setViewport);
  } else if (const auto* draw = std::get_if<DrawCommand>(&command)) {
    return encodeDraw(state, *draw);
  } else if (const auto* drawIndexed = std::get_if<DrawIndexedCommand>(&command)) {
    return encodeDrawIndexed(state, *drawIndexed);
  } else if (std::get_if<EndRenderPassCommand>(&command) != nullptr) {
    return encodeEndRenderPass(state);
  }
  return std::nullopt;
}

Status VulkanDevice::Impl::beginEncodedComputePass(EncodingState& state) {
  state.inComputePass = true;
  state.currentComputePipeline = nullptr;
  state.boundGroups.fill(nullptr);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeSetComputePipeline(EncodingState& state,
                                                    const SetComputePipelineCommand& setPipeline) {
  const ComputePipelineRecord* pipeline =
      FindRecord(computePipelines, setPipeline.pipelineId.slotIndex);
  if (!state.inComputePass || pipeline == nullptr || pipeline->pipeline == VK_NULL_HANDLE) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: compute pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  api->vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
  state.currentComputePipeline = pipeline;
  return OkStatus();
}

Status VulkanDevice::Impl::encodeDispatchWorkgroups(EncodingState& state,
                                                    const DispatchWorkgroupsCommand& dispatch) {
  if (!state.inComputePass || state.currentComputePipeline == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    "dispatchWorkgroups without an active compute pass and pipeline"};
  }
  // Bind every set the pipeline layout declares. The encoder's dispatch-time validation
  // guarantees each declared group index is bound; this re-check fails closed anyway.
  for (uint32_t setIndex = 0; setIndex < state.currentComputePipeline->layout->descriptorSetCount;
       ++setIndex) {
    const BindGroupRecord* group = state.boundGroups[setIndex];
    if (group == nullptr) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("dispatchWorkgroups: pipeline layout requires bind group {} but "
                                  "none is bound",
                                  setIndex)};
    }
    transitionBoundTextures(state, *group);
    api->vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 state.currentComputePipeline->layout->layout, setIndex, 1,
                                 &group->set, 0, nullptr);
  }
  api->vkCmdDispatch(state.commandBuffer, dispatch.workgroupCountX, dispatch.workgroupCountY,
                     dispatch.workgroupCountZ);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeEndComputePass(EncodingState& state) {
  if (!state.inComputePass) {
    return GpuError{GpuErrorType::InvalidState, "endComputePass without an active compute pass"};
  }
  // Storage writes are not automatically visible to later reads. The only writable binding the
  // runtime declares is a write-only storage texture, so a compute pass can write images and
  // nothing else, and the edge out of it names exactly that: shader writes made available to the
  // shader and transfer reads that can consume them. The per-image layout transition a later
  // consumer needs is recorded separately, when that consumer is encoded.
  //
  // If a writable storage BUFFER binding kind ever enters the runtime, this edge must gain the
  // host-visibility half - HOST_READ at the host stage - because every buffer this backend
  // allocates is host-mapped and persistently mapped, so a kernel write to one would otherwise
  // never become visible to the mapping the host reads through.
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  api->vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 1, &barrier, 0, nullptr, 0, nullptr);

  state.inComputePass = false;
  state.currentComputePipeline = nullptr;
  state.boundGroups.fill(nullptr);
  return OkStatus();
}

std::optional<Status> VulkanDevice::Impl::encodeComputeCommand(EncodingState& state,
                                                               std::span<const Command> commands,
                                                               size_t commandIndex) {
  const Command& command = commands[commandIndex];
  if (std::get_if<BeginComputePassCommand>(&command) != nullptr) {
    return beginEncodedComputePass(state);
  } else if (const auto* setPipeline = std::get_if<SetComputePipelineCommand>(&command)) {
    return encodeSetComputePipeline(state, *setPipeline);
  } else if (const auto* dispatch = std::get_if<DispatchWorkgroupsCommand>(&command)) {
    return encodeDispatchWorkgroups(state, *dispatch);
  } else if (std::get_if<EndComputePassCommand>(&command) != nullptr) {
    return encodeEndComputePass(state);
  }
  return std::nullopt;
}

Status VulkanDevice::Impl::encodeCopyTextureToTexture(EncodingState& state,
                                                      const CopyTextureToTextureCommand& copy) {
  if (state.inRenderPass) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToTexture inside a render pass"};
  }
  TextureRecord* source = FindRecord(textures, copy.textureSrcId.slotIndex);
  TextureRecord* destination = FindRecord(textures, copy.textureDstId.slotIndex);
  if (source == nullptr || destination == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToTexture: source or destination texture is missing"};
  }

  // Both operands move to their transfer layouts first. The staged layout is recorded for each so
  // a later pass's transition prescan starts from what this copy actually left behind, rather
  // than from the layout the texture was created in.
  transitionTexture(state.commandBuffer, copy.textureSrcId.slotIndex, *source,
                    TextureUsageKind::TransferRead);
  transitionTexture(state.commandBuffer, copy.textureDstId.slotIndex, *destination,
                    TextureUsageKind::TransferWrite);

  VkImageCopy copyRegion = {};
  copyRegion.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.srcOffset = VkOffset3D{static_cast<int32_t>(copy.sourceOrigin.x),
                                    static_cast<int32_t>(copy.sourceOrigin.y), 0};
  copyRegion.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.dstOffset = VkOffset3D{static_cast<int32_t>(copy.destinationOrigin.x),
                                    static_cast<int32_t>(copy.destinationOrigin.y), 0};
  copyRegion.extent = VkExtent3D{copy.copySize.width, copy.copySize.height, 1};
  api->vkCmdCopyImage(state.commandBuffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      destination->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
  return OkStatus();
}

std::optional<Status> VulkanDevice::Impl::encodeCopyCommand(EncodingState& state,
                                                            const Command& command) {
  if (const auto* copy = std::get_if<CopyTextureToBufferCommand>(&command)) {
    return encodeCopyTextureToBuffer(state, *copy);
  } else if (const auto* textureCopy = std::get_if<CopyTextureToTextureCommand>(&command)) {
    return encodeCopyTextureToTexture(state, *textureCopy);
  }
  return std::nullopt;
}

Status VulkanDevice::Impl::encodeCommand(EncodingState& state, std::span<const Command> commands,
                                         size_t commandIndex) {
  if (std::optional<Status> status = encodeRenderCommand(state, commands, commandIndex)) {
    return *status;
  }
  if (std::optional<Status> status = encodeComputeCommand(state, commands, commandIndex)) {
    return *status;
  }
  if (std::optional<Status> status = encodeCopyCommand(state, commands[commandIndex])) {
    return *status;
  }
  return OkStatus();
}

Status VulkanDevice::Impl::encodeCommands(EncodingState& state, std::span<const Command> commands) {
  for (size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
    if (Status status = encodeCommand(state, commands, commandIndex); status.hasError()) {
      return status;
    }
  }
  return OkStatus();
}

Status VulkanDevice::Impl::finishSubmission(uint64_t submissionSerial, EncodingState& state,
                                            VkFence fence) {
  const auto failEncoding = [&](Status error) -> Status {
    discardEncoding(state);
    return error;
  };
  // A frame acquired from a surface comes back before the presentation engine has finished
  // reading it, so the first submission that writes that frame carries its wait.
  const ClaimedSurfaceWaits claimed = claimSurfaceWaits(encodedTextureSlots);
  const VkResult submitResult = submitToQueue(state.commandBuffers, fence, claimed.sync);
  if (SubmissionWasRejected(submitResult)) {
    returnSurfaceWaits(claimed);
    api->vkDestroyFence(device, fence, nullptr);
    return failEncoding(VkError("vkQueueSubmit", submitResult));
  }
  if (submitResult == VK_ERROR_DEVICE_LOST && drainAfterDeviceLoss(submitResult, "vkQueueSubmit")) {
    api->vkDestroyFence(device, fence, nullptr);
    return failEncoding(VkError("vkQueueSubmit", submitResult));
  }
  if (submitResult != VK_SUCCESS) {
    executionUncertain = true;
    recordError(
        std::format("vkQueueSubmit completion unknown: {}", VkResultToString(submitResult)));
    syncStates.discardStaged();
  } else {
    syncStates.commitStaged();
  }

  InFlightSubmission submission;
  submission.serial = submissionSerial;
  submission.fence = fence;
  submission.commandBuffers = std::move(state.commandBuffers);
  submission.renderPasses = std::move(state.transientRenderPasses);
  submission.framebuffers = std::move(state.transientFramebuffers);
  submission.bufferWriteStaging = state.bufferWriteStaging;
  commitPendingBufferWrites(submissionSerial);
  inFlight.push_back(std::move(submission));
  return submitResult == VK_SUCCESS ? OkStatus() : Status(VkError("vkQueueSubmit", submitResult));
}

Status VulkanDevice::Impl::encodeSubmittedCommandBuffer(EncodingState& state,
                                                        std::span<const Command> commands,
                                                        bool encodeQueuedWrites) {
  Result<VkCommandBuffer> commandBufferResult = allocateCommandBuffer();
  if (commandBufferResult.hasError()) {
    return std::move(commandBufferResult).error();
  }
  state.commandBuffer = commandBufferResult.result();
  state.commandBuffers.push_back(state.commandBuffer);

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (const VkResult result = api->vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
      result != VK_SUCCESS) {
    return VkError("vkBeginCommandBuffer", result);
  }

  if (encodeQueuedWrites) {
    if (Status status = encodePendingBufferWrites(state); status.hasError()) {
      return status;
    }
  }

  if (Status status = encodeCommands(state, commands); status.hasError()) {
    return status;
  }

  if (state.inRenderPass || state.inComputePass) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    return GpuError{GpuErrorType::InvalidState, "submitted command stream left a pass open"};
  }

  if (const VkResult result = api->vkEndCommandBuffer(state.commandBuffer); result != VK_SUCCESS) {
    return VkError("vkEndCommandBuffer", result);
  }
  return OkStatus();
}

Status VulkanDevice::onSubmit(uint64_t submissionSerial,
                              std::span<const SubmittedCommandBuffer> commandBuffers) {
  Impl& impl = *impl_;

  // Transient objects created while encoding; on success they move into the in-flight record
  // and are destroyed when the fence signals, on failure they are destroyed here.
  Impl::EncodingState state;
  impl.encodedTextureSlots.clear();
  const auto failEncoding = [&](Status error) -> Status {
    // Recoverable failures leave the queue unchanged. Device loss can execute work, but its
    // layouts are undefined and the error latch prevents any later submission from using them.
    impl.discardEncoding(state);
    return error;
  };

  // The staged image layouts carry across the boundary between buffers, because a later buffer
  // of the same submission reads what an earlier one left behind: the barrier a sampled read
  // needs after a render pass wrote its texture is recorded in the buffer that samples it.
  for (size_t i = 0; i < commandBuffers.size(); ++i) {
    // Only the first buffer carries the queued writes: they are one staged batch for the whole
    // submission, and repeating them would copy each payload once per buffer.
    if (Status status =
            impl.encodeSubmittedCommandBuffer(state, commandBuffers[i].commands, i == 0);
        status.hasError()) {
      return failEncoding(std::move(status));
    }
  }

  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (const VkResult result = impl_->api->vkCreateFence(impl.device, &fenceInfo, nullptr, &fence);
      result != VK_SUCCESS) {
    return failEncoding(VkError("vkCreateFence", result));
  }

  return impl.finishSubmission(submissionSerial, state, fence);
}

// ---------------------------------------------------------------------------
// Host buffer mapping
// ---------------------------------------------------------------------------

std::span<const uint8_t> VulkanDevice::Impl::MappingHost::mappableBytes(
    uint32_t bufferSlotIndex) const {
  const Impl::BufferRecord* record = FindRecord(impl_.buffers, bufferSlotIndex);
  if (record == nullptr || record->allocation.mapped == nullptr) {
    return {};
  }
  // The allocator keeps every buffer host-visible, host-coherent and mapped for its lifetime, so
  // the pointer is already there and needs no flush or invalidate to read.
  return std::span<const uint8_t>(static_cast<const uint8_t*>(record->allocation.mapped),
                                  static_cast<size_t>(record->byteSize));
}

uint64_t VulkanDevice::Impl::MappingHost::completedSubmissionSerial() const {
  return device_.completedSerial();
}

MapWaitKind VulkanDevice::Impl::MappingHost::waitForSubmission(uint64_t serial,
                                                               double sliceSeconds) {
  if (device_.completedSerial() >= serial) {
    // Already done, so the wait below would return without blocking on anything; saying it used a
    // completion signal would credit the statistics with a wait that never happened.
    return MapWaitKind::Polled;
  }
  // vkWaitForFences blocks until the submission itself signals, so the slice is spent waiting on
  // a completion signal rather than rechecking readiness.
  (void)device_.waitForSerial(serial, sliceSeconds);
  return MapWaitKind::CompletionEvent;
}

bool VulkanDevice::Impl::MappingHost::deviceLost() const {
  // A loss any device over the root declares, including a bounded wait that gave up, ends a
  // mapping whether or not this device recorded an error of its own.
  return impl_.hasError() || device_.isLost();
}

Status VulkanDevice::onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex,
                                      MapMode /*mode*/, uint64_t offsetBytes, uint64_t byteCount) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  if (!impl_->mappingTable) {
    impl_->mappingHost.emplace(*this, *impl_);
    impl_->mappingTable.emplace(*impl_->mappingHost);
  }
  // A queued write has no serial yet: it is applied at the start of whichever submission happens
  // next, which can be unrelated work, so it would change the mapped bytes after the mapping had
  // already reported itself ready. Readiness cannot express that, so the mapping is refused until
  // the queue drains.
  if (impl_->hasPendingBufferWrite(bufferSlotIndex)) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("mapBufferAsync: buffer (slot {}) has a queued write that has not "
                                "been applied yet; submit before mapping",
                                bufferSlotIndex)};
  }
  const Impl::BufferRecord* record = FindRecord(impl_->buffers, bufferSlotIndex);
  // A mapping observes work that was already submitted, so the serial is taken now rather than
  // when the wait starts: a submission issued after this call belongs to a later mapping.
  const uint64_t readySerial =
      std::max(bufferLastUseSerial(bufferSlotIndex), record != nullptr ? record->uploadSerial : 0);
  return impl_->mappingTable->begin(mappingSlotIndex, bufferSlotIndex, offsetBytes, byteCount,
                                    readySerial);
}

MapSliceReport VulkanDevice::onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) {
  if (!impl_->mappingTable) {
    return MapSliceReport{.state = MapSliceState::Failed, .waitKind = MapWaitKind::Polled};
  }
  return impl_->mappingTable->waitSlice(mappingSlotIndex, sliceSeconds);
}

Result<std::span<const uint8_t>> VulkanDevice::onMappedBytes(uint32_t mappingSlotIndex) const {
  // No error check here: the mapping table answers a failed or lost device as a device loss,
  // which is what a read after the driver reported one must see.
  if (!impl_->mappingTable) {
    return GpuError{GpuErrorType::InvalidHandle, "mappedBytes: this device has no open mappings"};
  }
  return impl_->mappingTable->bytes(mappingSlotIndex);
}

void VulkanDevice::onUnmapBuffer(uint32_t mappingSlotIndex) {
  if (impl_->mappingTable) {
    impl_->mappingTable->release(mappingSlotIndex);
  }
}

// == Presentation ==============================================================================

bool VulkanDevice::supportsPresentation() const {
  return impl_->presentationEnabled;
}

void VulkanDevice::forceNextAcquireOutOfDateForTest(uint32_t surfaceSlotIndex) {
  if (VulkanSwapchain* surface = impl_->surfaceAt(surfaceSlotIndex); surface != nullptr) {
    surface->forceNextAcquireOutOfDateForTest();
  }
}

void VulkanDevice::forceMinimumImageCountOnceForTest(uint32_t surfaceSlotIndex) {
  if (VulkanSwapchain* surface = impl_->surfaceAt(surfaceSlotIndex); surface != nullptr) {
    surface->forceMinimumImageCountOnceForTest();
  }
}

VulkanDevice::SurfaceAcquisitionForTest VulkanDevice::surfaceAcquisitionForTest(
    uint32_t surfaceSlotIndex) const {
  const VulkanSwapchain* surface = impl_->surfaceAt(surfaceSlotIndex);
  if (surface == nullptr) {
    return SurfaceAcquisitionForTest{};
  }
  return SurfaceAcquisitionForTest{
      true, surface->owesAcquireWaitForTest(), surface->frameRingSlotForTest(),
      surface->lastFencedRingSlotForTest(), surface->acquireRingSizeForTest()};
}

void* VulkanDevice::nativeInstance() const {
  return impl_->presentationEnabled ? impl_->instance : nullptr;
}

Status VulkanDevice::onCreateSurface(uint32_t slotIndex, const SurfaceDescriptor& descriptor) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  if (!impl_->presentationEnabled) {
    return GpuError{GpuErrorType::Unsupported,
                    "createSurface: this device was created without presentation support"};
  }

  Result<std::unique_ptr<VulkanSwapchain>> surface =
      VulkanSwapchain::Create(impl_->surfaceContext(), descriptor);
  if (surface.hasError()) {
    return std::move(surface).error();
  }

  std::unique_ptr<VulkanSwapchain> swapchain = std::move(surface).result();
  // Ending a frame submits on the queue from inside the swapchain, outside `submit`; the observer
  // still sees it, as it sees every other submission this device makes.
  swapchain->setQueueSubmissionCallback([this] { notifyObserverOfBackendSubmission(); });
  SetSlot(impl_->surfaces, slotIndex, std::move(swapchain));
  SetSlot(impl_->surfaceTextureSlots, slotIndex, std::optional<uint32_t>());
  return OkStatus();
}

Result<SurfaceCapabilities> VulkanDevice::onSurfaceCapabilities(uint32_t slotIndex) const {
  const VulkanSwapchain* surface = impl_->surfaceAt(slotIndex);
  if (surface == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("surface slot {} has no Vulkan surface", slotIndex)};
  }
  return surface->capabilities();
}

Status VulkanDevice::onConfigureSurface(uint32_t slotIndex,
                                        const SurfaceConfiguration& configuration) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  VulkanSwapchain* surface = impl_->surfaceAt(slotIndex);
  if (surface == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("surface slot {} has no Vulkan surface", slotIndex)};
  }
  return surface->configure(configuration);
}

Result<SurfaceStatus> VulkanDevice::onAcquireCurrentTexture(uint32_t slotIndex,
                                                            uint32_t textureSlotIndex) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  VulkanSwapchain* surface = impl_->surfaceAt(slotIndex);
  if (surface == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("surface slot {} has no Vulkan surface", slotIndex)};
  }

  Result<SurfaceStatus> status = surface->acquire();
  if (status.hasError()) {
    return status;
  }

  const VkImage frameImage = surface->currentImage();
  if (frameImage == VK_NULL_HANDLE) {
    return status;  // No frame came back; the runtime releases the slot it proposed.
  }

  const SurfaceConfiguration& configuration = *surface->configuration();
  Impl::TextureRecord record;
  record.image = frameImage;
  record.memory = VK_NULL_HANDLE;
  record.format = configuration.format;
  record.size = surface->extent();
  record.usage = configuration.usage;
  // The swapchain owns its images and releases them with itself, so this record borrows.
  record.ownsImage = false;
  SetSlot(impl_->textures, textureSlotIndex, std::optional<Impl::TextureRecord>(record));
  // A frame comes out of the presentation engine with undefined contents, whatever the slot's
  // previous occupant was tracked in, and with the engine's own read as the last thing to have
  // touched it; the acquisition wait is what the first barrier has to be ordered after.
  impl_->syncStates.reset(textureSlotIndex, AcquiredFrameSyncState());
  SetSlot(impl_->surfaceTextureSlots, slotIndex, std::optional<uint32_t>(textureSlotIndex));
  surface->setFrameTextureSlot(textureSlotIndex);
  return status;
}

Result<SurfaceStatus> VulkanDevice::onPresentSurface(uint32_t slotIndex) {
  if (impl_->hasError()) {
    return GpuError{GpuErrorType::InvalidState, impl_->errorMessage()};
  }
  VulkanSwapchain* surface = impl_->surfaceAt(slotIndex);
  if (surface == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("surface slot {} has no Vulkan surface", slotIndex)};
  }

  const VkImage frameImage = surface->currentImage();
  const std::optional<uint32_t> textureSlot = slotIndex < impl_->surfaceTextureSlots.size()
                                                  ? impl_->surfaceTextureSlots[slotIndex]
                                                  : std::optional<uint32_t>();
  // What last touched the frame is the source scope of the barrier into the layout the
  // presentation engine reads; an untouched frame reports having touched nothing, which is
  // exactly the barrier a frame nobody drew into needs.
  TextureSyncState state;
  if (textureSlot.has_value()) {
    const Impl::TextureRecord* record = FindRecord(impl_->textures, *textureSlot);
    if (record == nullptr || record->image != frameImage) {
      const Status abandoned = surface->abandon();
      impl_->releaseFrameTextureSlot(slotIndex, frameImage);
      if (abandoned.hasError()) {
        return std::move(abandoned).error();
      }
      return GpuError{GpuErrorType::InvalidState,
                      "presentSurface: the acquired texture was released or replaced; its frame "
                      "was discarded"};
    }
    state = impl_->syncStates.committedStateOf(*textureSlot);
  }

  Result<SurfaceStatus> status = surface->present(state);
  impl_->releaseFrameTextureSlot(slotIndex, frameImage);
  return status;
}

void VulkanDevice::onAbandonCurrentTexture(uint32_t slotIndex) {
  if (impl_->executionUncertain || impl_->surfaceLifetimeUnproven()) {
    return;
  }
  VulkanSwapchain* surface = impl_->surfaceAt(slotIndex);
  if (surface == nullptr) {
    return;
  }

  const VkImage frameImage = surface->currentImage();
  if (Status status = surface->abandon(); status.hasError()) {
    impl_->recordError(status.error().message);
  }
  impl_->releaseFrameTextureSlot(slotIndex, frameImage);
}

void VulkanDevice::onDestroySurface(uint32_t slotIndex) {
  SetSlot(impl_->surfaceTextureSlots, slotIndex, std::optional<uint32_t>());
  if (slotIndex >= impl_->surfaces.size()) {
    return;
  }
  std::unique_ptr<VulkanSwapchain> surface = std::move(impl_->surfaces[slotIndex]);
  if (!surface) {
    return;
  }
  if (impl_->executionUncertain || impl_->surfaceLifetimeUnproven() ||
      surface->prepareForDestruction().hasError()) {
    surface->retainBefore(std::move(impl_->retainedSurfaces));
    impl_->retainedSurfaces = std::move(surface);
  }
}

}  // namespace donner::gpu::vulkan
