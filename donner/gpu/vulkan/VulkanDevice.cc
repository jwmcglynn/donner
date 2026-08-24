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
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan {

namespace {

/// Vulkan API version this backend targets (see the class comment: 1.1 core only).
constexpr uint32_t kTargetApiVersion = VK_API_VERSION_1_1;

/// Timeout for the synchronous internal texture-upload submission, in nanoseconds (60 s). A
/// stuck driver fails closed with an error instead of hanging the caller forever.
constexpr uint64_t kUploadFenceTimeoutNs = 60ull * 1000ull * 1000ull * 1000ull;

/// Validation layer enabled when the loader enumerates it (CI installs it explicitly; plain
/// driver installs usually do not have it, and it is skipped silently then).
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

/// Returns the name of a VkResult for diagnostics (the common codes; others print numerically).
std::string VkResultToString(VkResult result) {
  switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return std::format("VkResult({})", static_cast<int32_t>(result));
  }
}

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
    case BindingType::SampledTexture2dFloat: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::FilteringSampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
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

/// Records a conservative full image layout transition: ALL_COMMANDS to ALL_COMMANDS with
/// memory-availability source access and read+write destination access. Intentionally maximal
/// (the first implementation is conservative and validation-clean); VK_ACCESS_MEMORY_* flags are
/// valid with any pipeline stage.
/// @param api Resolved device entry points.
/// @param commandBuffer Command buffer to record into.
/// @param image Image whose layout changes.
/// @param oldLayout Layout the image is currently in.
/// @param newLayout Layout the image transitions to.
void RecordImageBarrier(const VulkanApi& api, VkCommandBuffer commandBuffer, VkImage image,
                        VkImageLayout oldLayout, VkImageLayout newLayout) {
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = FullColorRange();
  api.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
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
};

/// Opens the Vulkan loader, checks that it offers the API version this backend targets, and
/// creates the headless instance with the validation layer and debug-utils messenger extension
/// when they are available.
InstanceSetup CreateInstance() {
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

  // Vulkan 1.1 requires instance-level 1.1 support. On a 1.0-only loader
  // vkEnumerateInstanceVersion still exists as a loader export in loaders new enough for this
  // backend's deployment targets; a version below 1.1 fails closed here.
  uint32_t instanceVersion = 0;
  if (api.vkEnumerateInstanceVersion(&instanceVersion) != VK_SUCCESS ||
      instanceVersion < kTargetApiVersion) {
    return {};
  }

  // Enable the Khronos validation layer only when the loader enumerates it, and the debug-utils
  // messenger extension only alongside it: the messenger turns validation ERROR messages into
  // latched device errors (see ValidationMessengerCallback) instead of log lines.
  const std::vector<const char*> enabledLayers = EnumerateValidationLayer(api);
  const std::vector<const char*> enabledExtensions =
      EnumerateDebugUtilsExtension(api, !enabledLayers.empty());

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
    return {};
  }

  // Instance-level entry points exist only once an instance does. Nothing below may be called
  // before this succeeds; on failure the instance is unreachable except through the one entry
  // point that destroys it, so release it only when that one resolved.
  if (const Status status = loader->loadInstance(instance, !enabledExtensions.empty());
      status.hasError()) {
    std::fprintf(stderr, "[donner::gpu::vulkan] %s\n", status.error().message.c_str());
    if (api.vkDestroyInstance != nullptr) {
      api.vkDestroyInstance(instance, nullptr);
    }
    return {};
  }

  setup.loader = loader;
  setup.instance = instance;
  setup.debugMessengerAvailable = !enabledExtensions.empty();
  return setup;
}

}  // namespace

/// Vulkan state of a VulkanDevice: instance/device/queue handles plus per-resource slot tables
/// mirroring the validated slot indices handed to the `on*` hooks.
struct VulkanDevice::Impl {
  /// Keeps the Vulkan loader library open. Every entry point this device calls is code inside
  /// that library, so it must outlive every Vulkan object below.
  std::shared_ptr<VulkanLoader> loader;
  /// Entry points resolved for this instance and device; owned by \ref loader.
  const VulkanApi* api = nullptr;

  VkInstance instance = VK_NULL_HANDLE;                    //!< Owning instance; set by Create.
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;        //!< Selected physical device.
  VkPhysicalDeviceMemoryProperties memoryProperties = {};  //!< Memory heaps/types of the device.
  VkDevice device = VK_NULL_HANDLE;                        //!< Logical device.
  VkQueue queue = VK_NULL_HANDLE;                          //!< The single graphics queue.
  uint32_t queueFamilyIndex = 0;                           //!< Family index of \ref queue.
  VkCommandPool commandPool = VK_NULL_HANDLE;              //!< Pool for all command buffers.

  /// A buffer plus its dedicated allocation, persistently mapped (host-visible + coherent; see
  /// the class comment for why every buffer is host-visible in this slice).
  struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;        //!< Buffer handle.
    VkDeviceMemory memory = VK_NULL_HANDLE;  //!< Dedicated allocation.
    void* mapped = nullptr;                  //!< Persistent host mapping.
    VkDeviceSize byteSize = 0;               //!< Creation size in bytes.
  };

  /// An image plus its dedicated allocation and the CPU-tracked current layout. Layout tracking
  /// at encode time is valid because submissions execute in order on the single queue with
  /// conservative barriers.
  struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;                           //!< Image handle.
    VkDeviceMemory memory = VK_NULL_HANDLE;                   //!< Dedicated allocation.
    TextureFormat format = TextureFormat::RGBA8Unorm;         //!< RHI format (for copy texel math).
    Extent2d size;                                            //!< Extent in texels.
    TextureUsage usage = TextureUsage::None;                  //!< RHI usage flags.
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;  //!< Tracked layout.
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
  std::vector<std::optional<RenderPipelineRecord>> renderPipelines;    //!< Pipeline slots.

  /// One submitted command buffer awaiting fence completion, with the transient render passes
  /// and framebuffers its encoding created.
  struct InFlightSubmission {
    uint64_t serial = 0;                             //!< Submission serial.
    VkFence fence = VK_NULL_HANDLE;                  //!< Signaled when the submission completes.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;  //!< Submitted command buffer.
    std::vector<VkRenderPass> renderPasses;          //!< Transient per-pass render passes.
    std::vector<VkFramebuffer> framebuffers;         //!< Transient per-pass framebuffers.
  };

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

  /// Returns true once any failure was recorded.
  bool hasError() const { return errorState->hadError.load(std::memory_order_acquire); }

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
    for (VkFramebuffer framebuffer : submission.framebuffers) {
      api->vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    for (VkRenderPass renderPass : submission.renderPasses) {
      api->vkDestroyRenderPass(device, renderPass, nullptr);
    }
    if (submission.commandBuffer != VK_NULL_HANDLE) {
      api->vkFreeCommandBuffers(device, commandPool, 1, &submission.commandBuffer);
    }
    if (submission.fence != VK_NULL_HANDLE) {
      api->vkDestroyFence(device, submission.fence, nullptr);
    }
  }

  /// Polls pending fences in submission order, releasing completed submissions and advancing
  /// the monotonic completed-serial counter. Stops at the first unsignaled fence (fences on one
  /// queue signal in submission order).
  void pollCompleted() {
    size_t releasedCount = 0;
    for (InFlightSubmission& submission : inFlight) {
      const VkResult status = api->vkGetFenceStatus(device, submission.fence);
      if (status == VK_SUCCESS) {
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

  /// Allocates one primary command buffer from the pool.
  Result<VkCommandBuffer> allocateCommandBuffer() {
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

  /// Creates a buffer with a dedicated persistently-mapped host-visible allocation. Used for
  /// both RHI buffers and internal upload staging.
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

    VkMemoryRequirements requirements = {};
    api->vkGetBufferMemoryRequirements(device, record.buffer, &requirements);
    const std::optional<uint32_t> memoryType =
        FindMemoryType(memoryProperties, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!memoryType) {
      destroyBufferRecord(record);
      return GpuError{GpuErrorType::InvalidState,
                      std::format("no host-visible coherent memory type for buffer '{}'", label)};
    }

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = *memoryType;
    if (const VkResult result =
            api->vkAllocateMemory(device, &allocateInfo, nullptr, &record.memory);
        result != VK_SUCCESS) {
      destroyBufferRecord(record);
      return GpuError{GpuErrorType::InvalidState,
                      std::format("vkAllocateMemory of {} bytes for buffer '{}' failed with {}",
                                  requirements.size, label, VkResultToString(result))};
    }
    if (const VkResult result = api->vkBindBufferMemory(device, record.buffer, record.memory, 0);
        result != VK_SUCCESS) {
      destroyBufferRecord(record);
      return VkError("vkBindBufferMemory", result);
    }
    if (const VkResult result =
            api->vkMapMemory(device, record.memory, 0, VK_WHOLE_SIZE, 0, &record.mapped);
        result != VK_SUCCESS) {
      destroyBufferRecord(record);
      return VkError("vkMapMemory", result);
    }
    return record;
  }

  /// Destroys a buffer record's Vulkan objects (mapping is released implicitly by the free).
  void destroyBufferRecord(BufferRecord& record) {
    if (record.buffer != VK_NULL_HANDLE) {
      api->vkDestroyBuffer(device, record.buffer, nullptr);
      record.buffer = VK_NULL_HANDLE;
    }
    if (record.memory != VK_NULL_HANDLE) {
      api->vkFreeMemory(device, record.memory, nullptr);
      record.memory = VK_NULL_HANDLE;
    }
    record.mapped = nullptr;
  }

  /// Destroys a texture record's Vulkan objects.
  void destroyTextureRecord(TextureRecord& record) {
    if (record.image != VK_NULL_HANDLE) {
      api->vkDestroyImage(device, record.image, nullptr);
      record.image = VK_NULL_HANDLE;
    }
    if (record.memory != VK_NULL_HANDLE) {
      api->vkFreeMemory(device, record.memory, nullptr);
      record.memory = VK_NULL_HANDLE;
    }
  }

  /// Destroys every remaining Vulkan object in dependency-safe order. Called by the device
  /// destructor after in-flight submissions have completed (or timed out).
  void teardown() {
    if (device == VK_NULL_HANDLE) {
      destroyDebugMessenger();
      if (instance != VK_NULL_HANDLE) {
        api->vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
      }
      return;
    }

    for (InFlightSubmission& submission : inFlight) {
      releaseSubmission(submission);
    }
    inFlight.clear();

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
    pipelineLayouts.clear();  // Releases the remaining VkPipelineLayout handles.
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
  }

  /// Mutable state threaded through the encoding of one command stream.
  struct EncodingState {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;    //!< Command buffer being recorded.
    std::vector<VkRenderPass> transientRenderPasses;   //!< Render passes created while encoding.
    std::vector<VkFramebuffer> transientFramebuffers;  //!< Framebuffers created while encoding.
    bool inRenderPass = false;                         //!< True between begin and end pass.
    Extent2d passExtent;                               //!< Extent of the active pass.
    const RenderPipelineRecord* currentPipeline = nullptr;  //!< Pipeline bound in the pass.
    std::vector<VkDescriptorSet> boundSets;                 //!< Descriptor sets bound by index.
    /// Layout transitions recorded into this command buffer, committed to the per-texture
    /// tracked state only after vkQueueSubmit succeeds: a failed encode or submit must not leave
    /// tracked layouts claiming transitions the GPU never executed (the synchronous
    /// onWriteTexture upload follows the same commit-after-success pattern).
    std::map<uint32_t, VkImageLayout> stagedLayouts;
  };

  /// Returns the layout \p textureSlot will be in at this point of the encoding: the staged
  /// layout when one was recorded, otherwise the tracked layout.
  /// @param state Encoding state.
  /// @param textureSlot Texture slot to query.
  /// @param record Texture record backing the slot.
  VkImageLayout encodedLayoutOf(const EncodingState& state, uint32_t textureSlot,
                                const TextureRecord& record) const;

  /// Destroys the render passes, framebuffers, and command buffer created for an encoding that
  /// will not be submitted.
  /// @param state Encoding state.
  void destroyTransientEncodingObjects(EncodingState& state);

  /// Transitions every sampled texture the upcoming pass binds to the layout its descriptors
  /// declare. Barriers are illegal inside a render pass, so this pre-scans the pass's commands
  /// and records the transitions before the pass begins.
  /// @param state Encoding state.
  /// @param commands Full command stream.
  /// @param beginIndex Index of the BeginRenderPassCommand.
  void transitionPassSampledTextures(EncodingState& state, std::span<const Command> commands,
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

  /// Ends the active render pass and resets the per-pass binding state.
  /// @param state Encoding state.
  Status encodeEndRenderPass(EncodingState& state);

  /// Records a texture-to-buffer copy plus the barriers that make it visible to host reads.
  /// @param state Encoding state.
  /// @param copy Recorded command.
  Status encodeCopyTextureToBuffer(EncodingState& state, const CopyTextureToBufferCommand& copy);

  /// Encodes a render-pass command, or returns empty when the command is not one.
  /// @param state Encoding state.
  /// @param commands Full command stream, for the begin-pass pre-scan.
  /// @param commandIndex Index of the command to encode.
  std::optional<Status> encodeRenderCommand(EncodingState& state, std::span<const Command> commands,
                                            size_t commandIndex);

  /// Encodes a compute-pass command, or returns empty when \p command is not one.
  /// @param state Encoding state. @param command Recorded command.
  std::optional<Status> encodeComputeCommand(EncodingState& state, const Command& command);

  /// Encodes a copy command, or returns empty when \p command is not one.
  /// @param state Encoding state. @param command Recorded command.
  std::optional<Status> encodeCopyCommand(EncodingState& state, const Command& command);

  /// Encodes one recorded command.
  /// @param state Encoding state.
  /// @param commands Full command stream, for the begin-pass pre-scan.
  /// @param commandIndex Index of the command to encode.
  Status encodeCommand(EncodingState& state, std::span<const Command> commands,
                       size_t commandIndex);

  /// Commits the staged layout transitions to the tracked per-texture state, once the GPU is
  /// guaranteed to execute them.
  /// @param state Encoding state.
  void commitStagedLayouts(EncodingState& state);

  /// Destroys the transient objects of a staged upload, in reverse creation order.
  /// @param fence Upload fence, or null.
  /// @param commandBuffer Upload command buffer, or null.
  /// @param staging Staging buffer record.
  void destroyUploadObjects(VkFence fence, VkCommandBuffer commandBuffer, BufferRecord& staging);

  /// Records the staged buffer-to-image copy plus the layout transitions around it, and returns
  /// the layout the image is left in.
  /// @param commandBuffer Command buffer to record into.
  /// @param texture Destination texture record.
  /// @param stagingBuffer Buffer holding the upload bytes.
  /// @param dataLayout Row layout of the upload bytes.
  /// @param writeSize Destination extent in texels.
  VkImageLayout recordTextureUploadCopy(VkCommandBuffer commandBuffer, const TextureRecord& texture,
                                        VkBuffer stagingBuffer,
                                        const TexelCopyBufferLayout& dataLayout,
                                        const Extent2d& writeSize);

  /// Creates \p fence, submits \p commandBuffer on it, and waits for completion. On timeout the
  /// submission is still pending, so \p objectsStillInUse is set and the caller must not destroy
  /// the fence, the command buffer, or the staging buffer.
  /// @param commandBuffer Recorded upload command buffer.
  /// @param fence Set to the created fence.
  /// @param objectsStillInUse Set when the submission is still pending after the timeout.
  Status submitAndWaitTextureUpload(VkCommandBuffer commandBuffer, VkFence& fence,
                                    bool& objectsStillInUse);

  /// Destroys the Vulkan buffer backing \p slotIndex, if any.
  /// @param slotIndex Buffer slot.
  void destroyBufferSlot(uint32_t slotIndex);
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
  /// @param sampledTextureSlots Receives the texture slot of a sampled texture view entry.
  /// @param write Descriptor write to point at the resolved info.
  Status bindDescriptorResource(const BindGroupEntry& entry, size_t index,
                                std::vector<VkDescriptorBufferInfo>& bufferInfos,
                                std::vector<VkDescriptorImageInfo>& imageInfos,
                                std::vector<uint32_t>& sampledTextureSlots,
                                VkWriteDescriptorSet& write);
};

std::unique_ptr<VulkanDevice> VulkanDevice::Create() {
  InstanceSetup setup = CreateInstance();
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

  // WebGPU semantics require bounds-checked buffer access; robustBufferAccess is the Vulkan
  // feature that provides it, and its support is mandatory (the specification's "Features"
  // chapter), so requiring it cannot lose devices. Fail closed anyway if a broken
  // implementation reports it unsupported.
  VkPhysicalDeviceFeatures supportedFeatures = {};
  api.vkGetPhysicalDeviceFeatures(selectedDevice, &supportedFeatures);
  if (supportedFeatures.robustBufferAccess != VK_TRUE) {
    api.vkDestroyInstance(instance, nullptr);
    return nullptr;
  }
  VkPhysicalDeviceFeatures enabledFeatures = {};
  enabledFeatures.robustBufferAccess = VK_TRUE;

  const float queuePriority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo = {};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = selectedQueueFamily;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &queuePriority;

  // Vulkan 1.1 core only: no device extensions, and no optional features beyond the mandatory
  // robustBufferAccess (read-only storage buffer access in the fragment stage and
  // negative-height viewports are core).
  VkDeviceCreateInfo deviceInfo = {};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.pEnabledFeatures = &enabledFeatures;

  VkDevice device = VK_NULL_HANDLE;
  if (api.vkCreateDevice(selectedDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
    api.vkDestroyInstance(instance, nullptr);
    return nullptr;
  }

  // Device-level entry points come from the device itself, so every call this backend records
  // per frame reaches the implementation directly instead of through the loader's dispatch.
  if (const Status status = loader->loadDevice(device); status.hasError()) {
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
  impl.loader = loader;
  impl.api = &loader->api();
  impl.instance = instance;
  impl.physicalDevice = selectedDevice;
  impl.device = device;
  impl.queueFamilyIndex = selectedQueueFamily;
  impl.commandPool = commandPool;
  api.vkGetDeviceQueue(device, selectedQueueFamily, 0, &impl.queue);
  api.vkGetPhysicalDeviceMemoryProperties(selectedDevice, &impl.memoryProperties);

  if (setup.debugMessengerAvailable) {
    const DebugMessengerHandles messenger =
        CreateValidationMessenger(api, instance, impl.errorState.get());
    impl.destroyDebugMessengerFn = messenger.destroyFn;
    impl.debugMessenger = messenger.messenger;
  }
  return result;
}

VulkanDevice::VulkanDevice() : impl_(std::make_unique<Impl>()) {}

VulkanDevice::~VulkanDevice() {
  if (impl_->device != VK_NULL_HANDLE) {
    // Wait for in-flight submissions so deferred destructions drain before teardown. On timeout
    // (a hung submission), vkDeviceWaitIdle below is the last resort; teardown then proceeds.
    if (lastSubmittedSerial() > completedSerial()) {
      waitForSerial(lastSubmittedSerial(), /*timeoutSeconds=*/5.0);
    }
    impl_->api->vkDeviceWaitIdle(impl_->device);
    impl_->pollCompleted();
    poll();
  }
  impl_->teardown();
}

uint64_t VulkanDevice::completedSerial() const {
  // Single-threaded device (see the class comment): polling from a const accessor is safe, and
  // unique_ptr does not propagate const to the Impl.
  impl_->pollCompleted();
  return impl_->completedSerialValue;
}

bool VulkanDevice::waitForSerial(uint64_t serial, double timeoutSeconds) {
  Impl& impl = *impl_;
  impl.pollCompleted();
  if (impl.hasError()) {
    return false;
  }
  if (impl.completedSerialValue >= serial) {
    return true;
  }

  const Impl::InFlightSubmission* target = nullptr;
  for (const Impl::InFlightSubmission& submission : impl.inFlight) {
    if (submission.serial >= serial) {
      target = &submission;
      break;
    }
  }
  if (target == nullptr) {
    return false;  // Serial was never submitted.
  }

  const double clampedSeconds = timeoutSeconds > 0.0 ? timeoutSeconds : 0.0;
  const uint64_t timeoutNs = static_cast<uint64_t>(clampedSeconds * 1e9);
  const VkResult result =
      impl_->api->vkWaitForFences(impl.device, 1, &target->fence, VK_TRUE, timeoutNs);
  if (result == VK_TIMEOUT) {
    return false;
  }
  if (result != VK_SUCCESS) {
    impl.recordError(std::format("vkWaitForFences failed with {}", VkResultToString(result)));
    return false;
  }
  impl.pollCompleted();
  return !impl.hasError() && impl.completedSerialValue >= serial;
}

Result<std::vector<uint8_t>> VulkanDevice::readBackBuffer(const Buffer& buffer) {
  // Full handle validation (null, device identity, AND generation) through the base class, so a
  // stale handle whose slot was reused cannot read the replacement buffer.
  if (Status status = validateBufferHandleForBackend(buffer); status.hasError()) {
    return std::move(status).error();
  }
  const Impl::BufferRecord* record = FindRecord(impl_->buffers, buffer.slotIndex());
  if (record == nullptr || record->mapped == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("buffer handle (slot {}) does not name a live Vulkan buffer",
                                buffer.slotIndex())};
  }

  const uint8_t* contents = static_cast<const uint8_t*>(record->mapped);
  return std::vector<uint8_t>(contents, contents + record->byteSize);
}

std::string VulkanDevice::lastErrorForTest() const {
  return impl_->errorState->firstMessage();
}

Status VulkanDevice::onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) {
  Result<Impl::BufferRecord> record = impl_->createHostVisibleBuffer(
      descriptor.byteSize, ToVkBufferUsage(descriptor.usage), std::string_view(descriptor.label));
  if (record.hasError()) {
    return std::move(record).error();
  }
  SetSlot(impl_->buffers, slotIndex, std::optional<Impl::BufferRecord>(std::move(record).result()));
  return OkStatus();
}

Status VulkanDevice::onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) {
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
                                                  std::vector<uint32_t>& sampledTextureSlots,
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
    imageInfos[index] =
        VkDescriptorImageInfo{VK_NULL_HANDLE, view->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    write.pImageInfo = &imageInfos[index];
    // Snapshot the sampled texture slot so onSubmit can pre-transition it to the layout the
    // descriptor declares before a pass that binds this group.
    sampledTextureSlots.push_back(view->textureSlot);
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

    bindStatus = impl.bindDescriptorResource(entry, i, bufferInfos, imageInfos,
                                             record.sampledTextureSlots, write);
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
    destroyBufferRecord(*record);
    buffers[slotIndex].reset();
  }
}

void VulkanDevice::Impl::destroyTextureSlot(uint32_t slotIndex) {
  if (TextureRecord* record = FindRecord(textures, slotIndex)) {
    destroyTextureRecord(*record);
    textures[slotIndex].reset();
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
  (void)slotIndex;
  (void)descriptor;
  return GpuError{GpuErrorType::Unsupported,
                  "the Vulkan backend does not implement compute pipelines yet"};
}

void VulkanDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  Impl& impl = *impl_;
  // The base class defers this hook until every submission referencing the resource has
  // completed, so immediate destruction is safe. Unknown resource names are ignored; the base
  // class owns their bookkeeping.
  if (resourceName == "buffer") {
    impl.destroyBufferSlot(slotIndex);
  } else if (resourceName == "texture") {
    impl.destroyTextureSlot(slotIndex);
  } else if (resourceName == "textureView") {
    impl.destroyTextureViewSlot(slotIndex);
  } else if (resourceName == "sampler") {
    impl.destroySamplerSlot(slotIndex);
  } else if (resourceName == "bindGroupLayout") {
    impl.destroyBindGroupLayoutSlot(slotIndex);
  } else if (resourceName == "bindGroup") {
    impl.destroyBindGroupSlot(slotIndex);
  } else if (resourceName == "pipelineLayout") {
    impl.destroyPipelineLayoutSlot(slotIndex);
  } else if (resourceName == "shaderModule") {
    impl.destroyShaderModuleSlot(slotIndex);
  } else if (resourceName == "renderPipeline") {
    impl.destroyRenderPipelineSlot(slotIndex);
  }
}

Status VulkanDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                   std::span<const uint8_t> data) {
  const Impl::BufferRecord* record = FindRecord(impl_->buffers, slotIndex);
  if (record == nullptr || record->mapped == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no Vulkan buffer", slotIndex)};
  }
  if (!data.empty()) {
    std::memcpy(static_cast<uint8_t*>(record->mapped) + offsetBytes, data.data(), data.size());
  }
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

VkImageLayout VulkanDevice::Impl::recordTextureUploadCopy(VkCommandBuffer commandBuffer,
                                                          const TextureRecord& texture,
                                                          VkBuffer stagingBuffer,
                                                          const TexelCopyBufferLayout& dataLayout,
                                                          const Extent2d& writeSize) {
  RecordImageBarrier(*api, commandBuffer, texture.image, texture.currentLayout,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  const uint32_t texelSize = TextureFormatBytesPerTexel(texture.format);
  VkBufferImageCopy copyRegion = {};
  copyRegion.bufferOffset = dataLayout.offsetBytes;
  copyRegion.bufferRowLength = dataLayout.bytesPerRow / texelSize;  // In texels.
  copyRegion.bufferImageHeight = dataLayout.rowsPerImage;
  copyRegion.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.imageOffset = VkOffset3D{0, 0, 0};
  copyRegion.imageExtent = VkExtent3D{writeSize.width, writeSize.height, 1};
  api->vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, texture.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

  // Sampled textures move straight to their descriptor layout; others stay transfer-dst until a
  // later encode transitions them.
  const VkImageLayout postUploadLayout = HasAllFlags(texture.usage, TextureUsage::Sampled)
                                             ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  if (postUploadLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    RecordImageBarrier(*api, commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       postUploadLayout);
  }
  return postUploadLayout;
}

Status VulkanDevice::Impl::submitAndWaitTextureUpload(VkCommandBuffer commandBuffer, VkFence& fence,
                                                      bool& objectsStillInUse) {
  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (const VkResult result = api->vkCreateFence(device, &fenceInfo, nullptr, &fence);
      result != VK_SUCCESS) {
    return VkError("vkCreateFence", result);
  }

  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (const VkResult result = api->vkQueueSubmit(queue, 1, &submitInfo, fence);
      result != VK_SUCCESS) {
    return VkError("vkQueueSubmit (writeTexture)", result);
  }
  if (const VkResult result =
          api->vkWaitForFences(device, 1, &fence, VK_TRUE, kUploadFenceTimeoutNs);
      result != VK_SUCCESS) {
    if (result == VK_TIMEOUT) {
      // The submission is still pending: destroying its fence, command buffer, or staging
      // buffer now would violate their in-use requirements, trading a clean failure for
      // undefined behavior. Deliberately leak them and fail closed; the device destructor's
      // vkDeviceWaitIdle is the backstop before final teardown.
      objectsStillInUse = true;
      return VkError("vkWaitForFences (writeTexture, still pending; leaking upload objects)",
                     result);
    }
    return VkError("vkWaitForFences (writeTexture)", result);
  }
  return OkStatus();
}

Status VulkanDevice::onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                    const TexelCopyBufferLayout& dataLayout,
                                    const Extent2d& writeSize) {
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
  std::memcpy(staging.mapped, data.data(), data.size());

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

  const VkImageLayout postUploadLayout =
      impl.recordTextureUploadCopy(commandBuffer, *texture, staging.buffer, dataLayout, writeSize);

  if (const VkResult result = impl_->api->vkEndCommandBuffer(commandBuffer); result != VK_SUCCESS) {
    cleanup();
    return VkError("vkEndCommandBuffer", result);
  }

  bool objectsStillInUse = false;
  const Status submitStatus =
      impl.submitAndWaitTextureUpload(commandBuffer, fence, objectsStillInUse);
  if (submitStatus.hasError()) {
    if (!objectsStillInUse) {
      cleanup();
    }
    return submitStatus;
  }

  texture->currentLayout = postUploadLayout;
  cleanup();
  return OkStatus();
}

VkImageLayout VulkanDevice::Impl::encodedLayoutOf(const EncodingState& state, uint32_t textureSlot,
                                                  const TextureRecord& record) const {
  const auto it = state.stagedLayouts.find(textureSlot);
  return it != state.stagedLayouts.end() ? it->second : record.currentLayout;
}

void VulkanDevice::Impl::destroyTransientEncodingObjects(EncodingState& state) {
  for (VkFramebuffer framebuffer : state.transientFramebuffers) {
    api->vkDestroyFramebuffer(device, framebuffer, nullptr);
  }
  for (VkRenderPass renderPass : state.transientRenderPasses) {
    api->vkDestroyRenderPass(device, renderPass, nullptr);
  }
  // Freeing a command buffer in the recording state is legal; it has not been submitted.
  api->vkFreeCommandBuffers(device, commandPool, 1, &state.commandBuffer);
}

void VulkanDevice::Impl::transitionPassSampledTextures(EncodingState& state,
                                                       std::span<const Command> commands,
                                                       size_t beginIndex) {
  // An UNDEFINED oldLayout for a never-written texture is valid; its contents are undefined
  // either way.
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
    for (const uint32_t sampledSlot : scannedGroup->sampledTextureSlots) {
      const TextureRecord* sampled = FindRecord(textures, sampledSlot);
      if (sampled == nullptr) {
        continue;  // Submit-time re-validation makes this unreachable.
      }
      const VkImageLayout current = encodedLayoutOf(state, sampledSlot, *sampled);
      if (current != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        RecordImageBarrier(*api, state.commandBuffer, sampled->image, current,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        state.stagedLayouts[sampledSlot] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
    }
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

    // Conservative explicit transition to the attachment layout; the pass then begins and
    // ends in COLOR_ATTACHMENT_OPTIMAL.
    RecordImageBarrier(*api, state.commandBuffer, texture->image,
                       encodedLayoutOf(state, view->textureSlot, *texture),
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    state.stagedLayouts[view->textureSlot] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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

  // Conservative explicit external dependencies (the implicit defaults do not cover memory
  // access): everything-before -> color output, and color output -> everything-after.
  VkSubpassDependency dependencies[2] = {};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

  VkRenderPassCreateInfo renderPassInfo = {};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 2;
  renderPassInfo.pDependencies = dependencies;

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
  std::fill(state.boundSets.begin(), state.boundSets.end(), VK_NULL_HANDLE);
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
  if (!state.inRenderPass || bindGroup == nullptr || setBindGroup.index >= kMaxBindGroups) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setBindGroup: bind group slot {} is not encodable",
                                setBindGroup.bindGroupId.slotIndex)};
  }
  // Descriptor sets bind lazily at draw: vkCmdBindDescriptorSets needs the pipeline layout,
  // and the RHI allows setBindGroup before setPipeline.
  state.boundSets[setBindGroup.index] = bindGroup->set;
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

Status VulkanDevice::Impl::encodeDraw(EncodingState& state, const DrawCommand& draw) {
  if (!state.inRenderPass || state.currentPipeline == nullptr) {
    return GpuError{GpuErrorType::InvalidState, "draw without an active pass and pipeline"};
  }
  // Bind every set the pipeline layout declares. The encoder's draw-time validation
  // guarantees each declared group index is bound; this re-check fails closed anyway.
  for (uint32_t setIndex = 0; setIndex < state.currentPipeline->layout->descriptorSetCount;
       ++setIndex) {
    if (state.boundSets[setIndex] == VK_NULL_HANDLE) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("draw: pipeline layout requires bind group {} but none is bound", setIndex)};
    }
    api->vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 state.currentPipeline->layout->layout, setIndex, 1,
                                 &state.boundSets[setIndex], 0, nullptr);
  }
  api->vkCmdDraw(state.commandBuffer, draw.vertexCount, draw.instanceCount, draw.firstVertex,
                 draw.firstInstance);
  return OkStatus();
}

Status VulkanDevice::Impl::encodeEndRenderPass(EncodingState& state) {
  if (!state.inRenderPass) {
    return GpuError{GpuErrorType::InvalidState, "endRenderPass without an active render pass"};
  }
  api->vkCmdEndRenderPass(state.commandBuffer);
  state.inRenderPass = false;
  state.currentPipeline = nullptr;
  std::fill(state.boundSets.begin(), state.boundSets.end(), VK_NULL_HANDLE);
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

  RecordImageBarrier(*api, state.commandBuffer, texture->image,
                     encodedLayoutOf(state, copy.textureId.slotIndex, *texture),
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  state.stagedLayouts[copy.textureId.slotIndex] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

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

  // Make the transfer write visible to host reads (readback maps the buffer after the
  // fence): TRANSFER write -> HOST read.
  VkBufferMemoryBarrier bufferBarrier = {};
  bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufferBarrier.buffer = buffer->buffer;
  bufferBarrier.offset = 0;
  bufferBarrier.size = VK_WHOLE_SIZE;
  api->vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bufferBarrier, 0,
                            nullptr);
  return OkStatus();
}

std::optional<Status> VulkanDevice::Impl::encodeRenderCommand(EncodingState& state,
                                                              std::span<const Command> commands,
                                                              size_t commandIndex) {
  const Command& command = commands[commandIndex];
  if (const auto* beginPass = std::get_if<BeginRenderPassCommand>(&command)) {
    transitionPassSampledTextures(state, commands, commandIndex);
    return beginEncodedRenderPass(state, *beginPass);
  } else if (const auto* setPipeline = std::get_if<SetPipelineCommand>(&command)) {
    return encodeSetPipeline(state, *setPipeline);
  } else if (const auto* setBindGroup = std::get_if<SetBindGroupCommand>(&command)) {
    return encodeSetBindGroup(state, *setBindGroup);
  } else if (const auto* setVertexBuffer = std::get_if<SetVertexBufferCommand>(&command)) {
    return encodeSetVertexBuffer(state, *setVertexBuffer);
  } else if (const auto* setScissor = std::get_if<SetScissorRectCommand>(&command)) {
    return encodeSetScissorRect(state, *setScissor);
  } else if (const auto* setViewport = std::get_if<SetViewportCommand>(&command)) {
    return encodeSetViewport(state, *setViewport);
  } else if (const auto* draw = std::get_if<DrawCommand>(&command)) {
    return encodeDraw(state, *draw);
  } else if (std::get_if<EndRenderPassCommand>(&command) != nullptr) {
    return encodeEndRenderPass(state);
  }
  return std::nullopt;
}

std::optional<Status> VulkanDevice::Impl::encodeComputeCommand(EncodingState& state,
                                                               const Command& command) {
  (void)state;
  if (std::get_if<BeginComputePassCommand>(&command) != nullptr ||
      std::get_if<SetComputePipelineCommand>(&command) != nullptr ||
      std::get_if<DispatchWorkgroupsCommand>(&command) != nullptr ||
      std::get_if<EndComputePassCommand>(&command) != nullptr) {
    return Status(GpuError{GpuErrorType::Unsupported,
                           "the Vulkan backend does not implement compute passes yet"});
  }
  return std::nullopt;
}

std::optional<Status> VulkanDevice::Impl::encodeCopyCommand(EncodingState& state,
                                                            const Command& command) {
  if (const auto* copy = std::get_if<CopyTextureToBufferCommand>(&command)) {
    return encodeCopyTextureToBuffer(state, *copy);
  } else if (std::get_if<CopyTextureToTextureCommand>(&command) != nullptr) {
    return Status(GpuError{GpuErrorType::Unsupported,
                           "the Vulkan backend does not implement copyTextureToTexture yet"});
  }
  return std::nullopt;
}

Status VulkanDevice::Impl::encodeCommand(EncodingState& state, std::span<const Command> commands,
                                         size_t commandIndex) {
  if (std::optional<Status> status = encodeRenderCommand(state, commands, commandIndex)) {
    return *status;
  }
  if (std::optional<Status> status = encodeComputeCommand(state, commands[commandIndex])) {
    return *status;
  }
  if (std::optional<Status> status = encodeCopyCommand(state, commands[commandIndex])) {
    return *status;
  }
  return OkStatus();
}

void VulkanDevice::Impl::commitStagedLayouts(EncodingState& state) {
  for (const auto& [textureSlot, layout] : state.stagedLayouts) {
    if (TextureRecord* texture = FindRecord(textures, textureSlot)) {
      texture->currentLayout = layout;
    }
  }
}

Status VulkanDevice::onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                              std::span<const Command> commands) {
  (void)commandBufferSlotIndex;
  Impl& impl = *impl_;

  Result<VkCommandBuffer> commandBufferResult = impl.allocateCommandBuffer();
  if (commandBufferResult.hasError()) {
    return std::move(commandBufferResult).error();
  }

  // Transient objects created while encoding; on success they move into the in-flight record
  // and are destroyed when the fence signals, on failure they are destroyed here.
  Impl::EncodingState state;
  state.commandBuffer = commandBufferResult.result();
  state.boundSets.assign(kMaxBindGroups, VK_NULL_HANDLE);
  const auto failEncoding = [&](Status error) -> Status {
    impl.destroyTransientEncodingObjects(state);
    return error;
  };

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (const VkResult result = impl_->api->vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
      result != VK_SUCCESS) {
    return failEncoding(VkError("vkBeginCommandBuffer", result));
  }

  for (size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
    Status encodeStatus = impl.encodeCommand(state, commands, commandIndex);
    if (encodeStatus.hasError()) {
      return failEncoding(std::move(encodeStatus));
    }
  }

  if (state.inRenderPass) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    return failEncoding(
        GpuError{GpuErrorType::InvalidState, "submitted command stream left a render pass open"});
  }

  if (const VkResult result = impl_->api->vkEndCommandBuffer(state.commandBuffer);
      result != VK_SUCCESS) {
    return failEncoding(VkError("vkEndCommandBuffer", result));
  }

  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (const VkResult result = impl_->api->vkCreateFence(impl.device, &fenceInfo, nullptr, &fence);
      result != VK_SUCCESS) {
    return failEncoding(VkError("vkCreateFence", result));
  }

  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &state.commandBuffer;
  if (const VkResult result = impl_->api->vkQueueSubmit(impl.queue, 1, &submitInfo, fence);
      result != VK_SUCCESS) {
    impl_->api->vkDestroyFence(impl.device, fence, nullptr);
    return failEncoding(VkError("vkQueueSubmit", result));
  }

  // The GPU will execute the recorded transitions: commit the staged layouts to the tracked
  // per-texture state.
  impl.commitStagedLayouts(state);

  Impl::InFlightSubmission submission;
  submission.serial = submissionSerial;
  submission.fence = fence;
  submission.commandBuffer = state.commandBuffer;
  submission.renderPasses = std::move(state.transientRenderPasses);
  submission.framebuffers = std::move(state.transientFramebuffers);
  impl.inFlight.push_back(std::move(submission));
  return OkStatus();
}

}  // namespace donner::gpu::vulkan
