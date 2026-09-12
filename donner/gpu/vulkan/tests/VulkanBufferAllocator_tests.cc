/// @file
/// Buffer memory ownership and failure unwinding without a Vulkan driver.

#include "donner/gpu/vulkan/VulkanBufferAllocator.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan {
namespace {

template <typename Handle>
Handle FakeHandle(uintptr_t value) {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(value);
  } else {
    return static_cast<Handle>(value);
  }
}

class DedicatedBufferAllocatorTest : public testing::Test {
protected:
  void SetUp() override {
    active = this;
    properties.memoryTypeCount = 3;
    properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    properties.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    properties.memoryTypes[2].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    api.vkGetBufferMemoryRequirements = GetRequirements;
    api.vkAllocateMemory = Allocate;
    api.vkBindBufferMemory = Bind;
    api.vkMapMemory = Map;
    api.vkFreeMemory = Free;
  }

  void TearDown() override { active = nullptr; }

  void expectCalls(std::initializer_list<const char*> expected) {
    EXPECT_EQ(calls, std::vector<std::string>(expected.begin(), expected.end()));
  }

  void expectCleared(const BufferAllocation& allocation) {
    EXPECT_EQ(allocation.memory, VK_NULL_HANDLE);
    EXPECT_EQ(allocation.offsetBytes, 0u);
    EXPECT_EQ(allocation.mapped, nullptr);
    EXPECT_FALSE(allocation.ownsMemory);
  }

  static void VKAPI_CALL GetRequirements(VkDevice device, VkBuffer buffer,
                                         VkMemoryRequirements* requirements) {
    EXPECT_EQ(device, active->device);
    EXPECT_EQ(buffer, active->buffer);
    active->calls.emplace_back("requirements");
    requirements->size = active->bytes.size();
    requirements->alignment = 16;
    requirements->memoryTypeBits = active->typeBits;
  }

  static VkResult VKAPI_CALL Allocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                      const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    EXPECT_EQ(device, active->device);
    EXPECT_EQ(info->sType, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    EXPECT_EQ(info->allocationSize, active->bytes.size());
    EXPECT_EQ(info->memoryTypeIndex, active->expectedType);
    active->calls.emplace_back("allocate");
    if (active->allocationResult == VK_SUCCESS) {
      *memory = active->memory;
    }
    return active->allocationResult;
  }

  static VkResult VKAPI_CALL Bind(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                  VkDeviceSize offset) {
    EXPECT_EQ(device, active->device);
    EXPECT_EQ(buffer, active->buffer);
    EXPECT_EQ(memory, active->memory);
    EXPECT_EQ(offset, 0u);
    active->calls.emplace_back("bind");
    return active->bindResult;
  }

  static VkResult VKAPI_CALL Map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                 VkDeviceSize size, VkMemoryMapFlags flags, void** mapped) {
    EXPECT_EQ(device, active->device);
    EXPECT_EQ(memory, active->memory);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(size, VK_WHOLE_SIZE);
    EXPECT_EQ(flags, 0u);
    active->calls.emplace_back("map");
    if (active->mapResult == VK_SUCCESS) {
      *mapped = active->bytes.data();
    }
    return active->mapResult;
  }

  static void VKAPI_CALL Free(VkDevice device, VkDeviceMemory memory,
                              const VkAllocationCallbacks*) {
    EXPECT_EQ(device, active->device);
    EXPECT_EQ(memory, active->memory);
    active->calls.emplace_back("free");
  }

  inline static DedicatedBufferAllocatorTest* active = nullptr;
  VulkanApi api;
  VkPhysicalDeviceMemoryProperties properties{};
  VkDevice device = FakeHandle<VkDevice>(1);
  VkBuffer buffer = FakeHandle<VkBuffer>(2);
  VkDeviceMemory memory = FakeHandle<VkDeviceMemory>(3);
  std::array<uint8_t, 256> bytes{};
  std::vector<std::string> calls;
  uint32_t typeBits = 0b111;
  uint32_t expectedType = 2;
  VkResult allocationResult = VK_SUCCESS;
  VkResult bindResult = VK_SUCCESS;
  VkResult mapResult = VK_SUCCESS;
};

TEST_F(DedicatedBufferAllocatorTest, SelectsCoherentVisibleMemoryAndReleasesExactlyOnce) {
  DedicatedBufferAllocator allocator(properties);
  auto result = allocator.allocate(api, device, buffer, "uniforms");
  ASSERT_TRUE(result.hasResult());
  BufferAllocation allocation = result.result();
  EXPECT_EQ(allocation.memory, memory);
  EXPECT_EQ(allocation.offsetBytes, 0u);
  EXPECT_EQ(allocation.mapped, bytes.data());
  EXPECT_TRUE(allocation.ownsMemory);
  expectCalls({"requirements", "allocate", "bind", "map"});

  allocator.release(api, device, allocation);
  expectCleared(allocation);
  expectCalls({"requirements", "allocate", "bind", "map", "free"});
  allocator.release(api, device, allocation);
  expectCleared(allocation);
  expectCalls({"requirements", "allocate", "bind", "map", "free"});
}

TEST_F(DedicatedBufferAllocatorTest, ResourceTypeMaskCannotSelectIncompatibleMemory) {
  // The coherent+visible type exists, but this buffer cannot use it. Neither
  // of the permitted one-property types can support persistent coherent writes.
  typeBits = 0b011;
  DedicatedBufferAllocator allocator(properties);
  auto result = allocator.allocate(api, device, buffer, "uniforms");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().type, GpuErrorType::InvalidState);
  EXPECT_NE(result.error().message.find("no host-visible coherent memory type"), std::string::npos);
  expectCalls({"requirements"});
}

TEST_F(DedicatedBufferAllocatorTest, MemoryTypeMaskSelectsALaterCompatibleType) {
  properties.memoryTypeCount = 4;
  properties.memoryTypes[3] = properties.memoryTypes[2];
  typeBits = 0b1000;
  expectedType = 3;
  DedicatedBufferAllocator allocator(properties);
  auto result = allocator.allocate(api, device, buffer, "uniforms");
  ASSERT_TRUE(result.hasResult());
  BufferAllocation allocation = result.result();
  allocator.release(api, device, allocation);
  expectCalls({"requirements", "allocate", "bind", "map", "free"});
}

TEST_F(DedicatedBufferAllocatorTest, AllocationFailureDoesNotBindMapOrFreeUnownedMemory) {
  allocationResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  DedicatedBufferAllocator allocator(properties);
  auto result = allocator.allocate(api, device, buffer, "uniforms");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().type, GpuErrorType::InvalidState);
  EXPECT_NE(result.error().message.find("vkAllocateMemory"), std::string::npos);
  expectCalls({"requirements", "allocate"});
}

TEST_F(DedicatedBufferAllocatorTest, BindingFailureReleasesAcquiredMemoryWithoutMapping) {
  bindResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  DedicatedBufferAllocator allocator(properties);
  auto result = allocator.allocate(api, device, buffer, "uniforms");
  ASSERT_TRUE(result.hasError());
  EXPECT_NE(result.error().message.find("vkBindBufferMemory"), std::string::npos);
  expectCalls({"requirements", "allocate", "bind", "free"});
}

TEST_F(DedicatedBufferAllocatorTest, MappingFailureReleasesAcquiredMemoryOnce) {
  mapResult = VK_ERROR_MEMORY_MAP_FAILED;
  DedicatedBufferAllocator allocator(properties);
  auto result = allocator.allocate(api, device, buffer, "uniforms");
  ASSERT_TRUE(result.hasError());
  EXPECT_NE(result.error().message.find("vkMapMemory"), std::string::npos);
  expectCalls({"requirements", "allocate", "bind", "map", "free"});
}

TEST_F(DedicatedBufferAllocatorTest, ReleasingBorrowedMemoryClearsTheRecordWithoutFreeingIt) {
  DedicatedBufferAllocator allocator(properties);
  BufferAllocation allocation{memory, 64, bytes.data() + 64, false};
  allocator.release(api, device, allocation);
  expectCleared(allocation);
  expectCalls({});
}

}  // namespace
}  // namespace donner::gpu::vulkan
