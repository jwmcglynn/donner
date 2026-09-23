#pragma once
/// @file
/// \c donner::gpu::vulkan::tests::NativeQueueGate - holds the device queue busy from the test
/// side, so a buffer stays in flight for as long as a test needs it to.
///
/// Submissions on this backend complete in microseconds under a software rasterizer, so a test
/// that needs a resource to still be busy cannot get there by submitting work and hoping. The
/// gate submits a command buffer that waits on a timeline semaphore the test owns: nothing the
/// device submits afterwards can retire until \ref release is called.

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <cstdint>

#include "donner/gpu/vulkan/VulkanDevice.h"
#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan::tests {

/// A test-owned timeline semaphore blocks later queue work until explicitly released.
class NativeQueueGate {
public:
  explicit NativeQueueGate(VulkanDevice::NativeContextForTest context)
      : api_(*context.api),
        device_(static_cast<VkDevice>(context.device)),
        queue_(static_cast<VkQueue>(context.queue)),
        family_(context.queueFamilyIndex) {
    createSemaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(
        api_.vkGetDeviceProcAddr(device_, "vkCreateSemaphore"));
    destroySemaphore_ = reinterpret_cast<PFN_vkDestroySemaphore>(
        api_.vkGetDeviceProcAddr(device_, "vkDestroySemaphore"));
    signalSemaphore_ = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        api_.vkGetDeviceProcAddr(device_, "vkSignalSemaphoreKHR"));
  }

  ~NativeQueueGate() {
    if (semaphore_ != VK_NULL_HANDLE) {
      EXPECT_EQ(release(), VK_SUCCESS);
    }
    if (submitted_) {
      const VkResult result = api_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000);
      if (result != VK_SUCCESS) {
        ADD_FAILURE() << "Released native gate did not finish: " << result;
        return;  // Retain possibly in-use objects until the device is destroyed.
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      api_.vkDestroyCommandPool(device_, pool_, nullptr);
    }
    if (fence_ != VK_NULL_HANDLE) {
      api_.vkDestroyFence(device_, fence_, nullptr);
    }
    if (semaphore_ != VK_NULL_HANDLE) {
      destroySemaphore_(device_, semaphore_, nullptr);
    }
  }

  void start() {
    ASSERT_NE(createSemaphore_, nullptr);
    ASSERT_NE(destroySemaphore_, nullptr);
    ASSERT_NE(signalSemaphore_, nullptr);
    VkSemaphoreTypeCreateInfoKHR type = {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &type;
    ASSERT_EQ(createSemaphore_(device_, &semaphoreInfo, nullptr, &semaphore_), VK_SUCCESS);
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family_;
    ASSERT_EQ(api_.vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_), VK_SUCCESS);
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ASSERT_EQ(api_.vkCreateFence(device_, &fenceInfo, nullptr, &fence_), VK_SUCCESS);
    recordAndSubmit();
  }

  VkResult release() {
    if (released_) {
      return VK_SUCCESS;
    }
    VkSemaphoreSignalInfoKHR signal = {};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO_KHR;
    signal.semaphore = semaphore_;
    signal.value = 1;
    const VkResult result = signalSemaphore_(device_, &signal);
    released_ = result == VK_SUCCESS;
    return result;
  }
  bool submitted() const { return submitted_; }

private:
  void recordAndSubmit() {
    VkCommandBufferAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    ASSERT_EQ(api_.vkAllocateCommandBuffers(device_, &allocate, &command), VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ASSERT_EQ(api_.vkBeginCommandBuffer(command, &begin), VK_SUCCESS);
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    // The tested host write happens after queue submission, so it needs an explicit domain
    // transfer.
    api_.vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    ASSERT_EQ(api_.vkEndCommandBuffer(command), VK_SUCCESS);
    const uint64_t waitValue = 1;
    VkTimelineSemaphoreSubmitInfoKHR timeline = {};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR;
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &waitValue;
    const VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &semaphore_;
    submit.pWaitDstStageMask = &waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    ASSERT_EQ(api_.vkQueueSubmit(queue_, 1, &submit, fence_), VK_SUCCESS);
    submitted_ = true;
  }

  const VulkanApi& api_;
  VkDevice device_;
  VkQueue queue_;
  uint32_t family_;
  PFN_vkCreateSemaphore createSemaphore_ = nullptr;
  PFN_vkDestroySemaphore destroySemaphore_ = nullptr;
  PFN_vkSignalSemaphoreKHR signalSemaphore_ = nullptr;
  VkSemaphore semaphore_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool submitted_ = false;
  bool released_ = false;
};

}  // namespace donner::gpu::vulkan::tests
