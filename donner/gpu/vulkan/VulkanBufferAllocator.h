#pragma once
/// @file
/// \c donner::gpu::vulkan::BufferSuballocator - where a Vulkan buffer's memory comes from.
///
/// The backend gives every buffer its own VkDeviceMemory, persistently mapped. That is the
/// simplest thing that is correct, and it is what this seam's only implementation still does.
///
/// The seam exists because replacing it is a memory-management change, not an API change, and
/// the two should not have to happen together. A device has a small, driver-reported cap on how
/// many allocations may exist at once, and one allocation per buffer walks toward that cap in
/// proportion to how many buffers a scene needs; suballocating many buffers out of a few larger
/// allocations is the standard answer. Making that swap later means writing another
/// implementation of this interface, not touching every call site that creates a buffer.
///
/// It is deliberately introduced without the suballocating implementation: which allocation
/// sizes, which pooling, and whether the cap is actually the binding constraint here are
/// questions for measurement, and shipping a suballocator before that measurement would be
/// guessing at a policy no counter has justified yet.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string_view>

#include "donner/gpu/GpuResult.h"

namespace donner::gpu::vulkan {

struct VulkanApi;

/// Where one buffer's bytes live.
///
/// The offset and the host pointer are the buffer's own, not the allocation's: a suballocating
/// implementation hands out many of these pointing into one \ref memory, so a caller must never
/// assume a buffer starts at offset zero of its allocation.
struct BufferAllocation {
  VkDeviceMemory memory = VK_NULL_HANDLE;  //!< Allocation this buffer was bound into.
  VkDeviceSize offsetBytes = 0;            //!< Offset of this buffer within \ref memory.
  void* mapped = nullptr;                  //!< Host pointer to this buffer's first byte.

  /// Whether releasing this allocation frees \ref memory. False for a suballocation that shares
  /// its memory with other buffers still in use.
  bool ownsMemory = false;
};

/**
 * Supplies the memory a buffer is bound into.
 *
 * Every buffer this backend allocates is host-visible, host-coherent, and persistently mapped:
 * queue writes are a memcpy and readback needs no staging. An implementation must preserve that,
 * because the rest of the backend reads and writes buffers straight through
 * \ref BufferAllocation::mapped with no flush or invalidate of its own.
 */
class BufferSuballocator {
public:
  virtual ~BufferSuballocator() = default;

  /**
   * Provides memory for \p buffer and binds it.
   *
   * @param api Resolved device entry points.
   * @param device Device owning \p buffer.
   * @param buffer Buffer to bind memory into.
   * @param label Buffer label, for failure messages.
   */
  [[nodiscard]] virtual Result<BufferAllocation> allocate(const VulkanApi& api, VkDevice device,
                                                          VkBuffer buffer,
                                                          std::string_view label) = 0;

  /**
   * Releases an allocation obtained from \ref allocate.
   *
   * @param api Resolved device entry points.
   * @param device Device owning the allocation.
   * @param allocation Allocation to release; left cleared.
   */
  virtual void release(const VulkanApi& api, VkDevice device, BufferAllocation& allocation) = 0;
};

/// The passthrough implementation: one dedicated VkDeviceMemory per buffer, mapped for its
/// lifetime. Every allocation it returns owns its memory and starts at offset zero.
class DedicatedBufferAllocator final : public BufferSuballocator {
public:
  /// @param memoryProperties Physical device memory properties, for memory-type selection.
  explicit DedicatedBufferAllocator(const VkPhysicalDeviceMemoryProperties& memoryProperties)
      : memoryProperties_(memoryProperties) {}

  Result<BufferAllocation> allocate(const VulkanApi& api, VkDevice device, VkBuffer buffer,
                                    std::string_view label) override;
  void release(const VulkanApi& api, VkDevice device, BufferAllocation& allocation) override;

private:
  VkPhysicalDeviceMemoryProperties memoryProperties_;  //!< Memory types available.
};

}  // namespace donner::gpu::vulkan
