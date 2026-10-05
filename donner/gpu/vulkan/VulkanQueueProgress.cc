#include "donner/gpu/vulkan/VulkanQueueProgress.h"

#include <algorithm>
#include <format>

namespace donner::gpu::vulkan {

VulkanQueueProgress::VulkanQueueProgress(const VulkanApi& api, VkDevice device)
    : api_(api), device_(device), lastProgress_(std::chrono::steady_clock::now()) {}

VulkanQueueProgress::~VulkanQueueProgress() {
  for (const std::vector<VkEvent>* events : {&free_, &pending_}) {
    for (VkEvent event : *events) {
      api_.vkDestroyEvent(device_, event, nullptr);
    }
  }
}

Result<VkEvent> VulkanQueueProgress::acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!free_.empty()) {
    const VkEvent event = free_.back();
    free_.pop_back();
    return event;
  }
  VkEventCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
  VkEvent event = VK_NULL_HANDLE;
  if (const VkResult result = api_.vkCreateEvent(device_, &info, nullptr, &event);
      result != VK_SUCCESS) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("vkCreateEvent failed with {}", VkResultToString(result))};
  }
  return event;
}

void VulkanQueueProgress::releaseUnsubmitted(std::span<const VkEvent> events) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (VkEvent event : events) {
    // Reset in case the queue ran the work anyway: a submission the driver reported lost is
    // drained before its objects come back, and that work may have set its events.
    (void)api_.vkResetEvent(device_, event);
    free_.push_back(event);
  }
}

void VulkanQueueProgress::noteSubmitted(std::span<const VkEvent> events) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_.empty()) {
    // Nothing tracked was outstanding, so a stall in this work is measured from now rather than
    // from progress long before it. Work that sets no event starts the clock too: it is still
    // work a wait may be waiting for.
    noteProgressLocked();
  }
  pending_.insert(pending_.end(), events.begin(), events.end());
}

void VulkanQueueProgress::retire(std::span<const VkEvent> events) {
  if (events.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (VkEvent event : events) {
    if (const auto found = std::ranges::find(pending_, event); found != pending_.end()) {
      // The submission completed before any wait saw this event set: that completion is
      // progress, seen now.
      *found = pending_.back();
      pending_.pop_back();
      noteProgressLocked();
    }
    // The submission that set it has completed, so no pending command buffer refers to it.
    (void)api_.vkResetEvent(device_, event);
    free_.push_back(event);
  }
}

std::chrono::steady_clock::time_point VulkanQueueProgress::observe() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < pending_.size();) {
    if (api_.vkGetEventStatus(device_, pending_[i]) == VK_EVENT_SET) {
      pending_[i] = pending_.back();
      pending_.pop_back();
      noteProgressLocked();
    } else {
      ++i;
    }
  }
  return lastProgress_;
}

void VulkanQueueProgress::noteProgressLocked() {
  lastProgress_ = std::chrono::steady_clock::now();
}

std::ostream& operator<<(std::ostream& os, FenceProgressWaitEnd value) {
  switch (value) {
    case FenceProgressWaitEnd::Signalled: return os << "Signalled";
    case FenceProgressWaitEnd::Stalled: return os << "Stalled";
    case FenceProgressWaitEnd::RootLost: return os << "RootLost";
    case FenceProgressWaitEnd::Failed: return os << "Failed";
  }
  return os << "Unknown";
}

FenceProgressWait WaitForFencesWhileQueueProgresses(const VulkanApi& api, VkDevice device,
                                                    std::span<const VkFence> fences,
                                                    VulkanQueueProgress* progress,
                                                    const DeviceLostState* rootLoss,
                                                    std::chrono::steady_clock::duration stallBound,
                                                    const std::function<void()>& stepHook) {
  using Clock = std::chrono::steady_clock;
  constexpr std::chrono::nanoseconds kStepInterval = std::chrono::milliseconds(10);
  const Clock::time_point waitStart = Clock::now();
  const auto ended = [](FenceProgressWaitEnd end, VkResult result) {
    return FenceProgressWait{end, result};
  };
  const Clock::duration stallBoundClock = std::max(stallBound, Clock::duration::zero());
  VkResult result = VK_TIMEOUT;
  if (fences.empty()) {
    return ended(FenceProgressWaitEnd::Signalled, VK_SUCCESS);
  }
  const auto fenceCount = static_cast<uint32_t>(fences.size());
  if (progress == nullptr && rootLoss == nullptr) {
    // Nothing to look at between steps, so one wait for the whole bound does the same.
    result = api.vkWaitForFences(
        device, fenceCount, fences.data(), VK_TRUE,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stallBoundClock).count()));
    return ended(result == VK_SUCCESS   ? FenceProgressWaitEnd::Signalled
                 : result == VK_TIMEOUT ? FenceProgressWaitEnd::Stalled
                                        : FenceProgressWaitEnd::Failed,
                 result);
  }
  for (;;) {
    const Clock::time_point progressAt = progress != nullptr ? progress->observe() : waitStart;
    const Clock::time_point stallDeadline = progressAt + stallBoundClock;
    const std::chrono::nanoseconds remaining = std::max(
        std::chrono::nanoseconds::zero(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(stallDeadline - Clock::now()));
    // A step of zero still asks once, so a fence that has signalled is never reported stalled.
    const std::chrono::nanoseconds step = std::min(remaining, kStepInterval);
    result = api.vkWaitForFences(device, fenceCount, fences.data(), VK_TRUE,
                                 static_cast<uint64_t>(step.count()));
    if (result == VK_SUCCESS) {
      return ended(FenceProgressWaitEnd::Signalled, result);
    }
    if (result != VK_TIMEOUT) {
      return ended(FenceProgressWaitEnd::Failed, result);
    }
    if (stepHook) {
      stepHook();
    }
    if (rootLoss != nullptr && rootLoss->lost.load(std::memory_order_acquire)) {
      return ended(FenceProgressWaitEnd::RootLost, result);
    }
    if (remaining <= step) {
      // The bound ran out on this step. Look once more: progress seen during it moves the
      // deadline, and only a queue that has still not progressed is stalled.
      const Clock::time_point latest = progress != nullptr ? progress->observe() : waitStart;
      if (Clock::now() - latest >= stallBoundClock) {
        return ended(FenceProgressWaitEnd::Stalled, result);
      }
    }
  }
}

}  // namespace donner::gpu::vulkan
