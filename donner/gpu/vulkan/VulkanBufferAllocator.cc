/// @file
/// Implementation of the Vulkan backend's buffer memory seam.

#include "donner/gpu/vulkan/VulkanBufferAllocator.h"

#include <format>
#include <optional>

#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan {

namespace {

/// Finds a memory type index compatible with \p typeBits carrying all \p required property
/// flags, or empty if none exists.
/// @param properties Physical device memory properties.
/// @param typeBits Memory type bitmask the resource accepts.
/// @param required Property flags the type must carry.
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

}  // namespace

Result<BufferAllocation> DedicatedBufferAllocator::allocate(const VulkanApi& api, VkDevice device,
                                                            VkBuffer buffer,
                                                            std::string_view label) {
  VkMemoryRequirements requirements = {};
  api.vkGetBufferMemoryRequirements(device, buffer, &requirements);

  // Host-visible and host-coherent, which the specification guarantees at least one memory type
  // provides. Coherent memory is what lets the backend read and write through the persistent
  // mapping with no flush or invalidate of its own.
  const std::optional<uint32_t> memoryType =
      FindMemoryType(memoryProperties_, requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!memoryType) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("no host-visible coherent memory type for buffer '{}'", label)};
  }

  BufferAllocation allocation;
  allocation.ownsMemory = true;
  allocation.offsetBytes = 0;

  VkMemoryAllocateInfo allocateInfo = {};
  allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocateInfo.allocationSize = requirements.size;
  allocateInfo.memoryTypeIndex = *memoryType;
  if (const VkResult result =
          api.vkAllocateMemory(device, &allocateInfo, nullptr, &allocation.memory);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkAllocateMemory of {} bytes for buffer '{}' failed with {}",
                                requirements.size, label, static_cast<int>(result))};
  }
  if (const VkResult result =
          api.vkBindBufferMemory(device, buffer, allocation.memory, allocation.offsetBytes);
      result != VK_SUCCESS) {
    release(api, device, allocation);
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkBindBufferMemory for buffer '{}' failed with {}", label,
                                static_cast<int>(result))};
  }
  if (const VkResult result =
          api.vkMapMemory(device, allocation.memory, 0, VK_WHOLE_SIZE, 0, &allocation.mapped);
      result != VK_SUCCESS) {
    release(api, device, allocation);
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("vkMapMemory for buffer '{}' failed with {}", label, static_cast<int>(result))};
  }
  return allocation;
}

void DedicatedBufferAllocator::release(const VulkanApi& api, VkDevice device,
                                       BufferAllocation& allocation) {
  // The mapping is released implicitly with the memory it belongs to.
  if (allocation.ownsMemory && allocation.memory != VK_NULL_HANDLE) {
    api.vkFreeMemory(device, allocation.memory, nullptr);
  }
  allocation = BufferAllocation{};
}

}  // namespace donner::gpu::vulkan
