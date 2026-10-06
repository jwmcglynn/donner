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

namespace {

using Clock = std::chrono::steady_clock;

/// Longest single native wait between looks for progress and for a declared loss.
constexpr std::chrono::nanoseconds kStepInterval = std::chrono::milliseconds(10);

/// One native wait for every fence in \p fences, for up to \p timeout.
/// @param api Entry points. @param device Device the fences belong to.
/// @param fences Fences to wait for. @param timeout Longest to wait.
VkResult WaitForAllFences(const VulkanApi& api, VkDevice device, std::span<const VkFence> fences,
                          std::chrono::nanoseconds timeout) {
  return api.vkWaitForFences(device, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE,
                             static_cast<uint64_t>(timeout.count()));
}

/// How a wait ends on a native result that is not a timeout. @param result Native result.
FenceProgressWaitEnd EndForAnsweredWait(VkResult result) {
  return result == VK_SUCCESS ? FenceProgressWaitEnd::Signalled : FenceProgressWaitEnd::Failed;
}

/// Whether a loss has been declared over the root. @param rootLoss Loss condition, or null.
bool RootDeclaredLost(const DeviceLostState* rootLoss) {
  return rootLoss != nullptr && rootLoss->lost.load(std::memory_order_acquire);
}

/// Time left before the queue has gone \p stallBound without progress, or zero once it has.
/// Looks for progress first. Without a progress record, the bound runs from \p waitStart.
/// @param progress Progress of the queue, or null. @param waitStart When the wait began.
/// @param stallBound Longest the queue may go without progress.
std::chrono::nanoseconds TimeUntilStalled(VulkanQueueProgress* progress,
                                          Clock::time_point waitStart, Clock::duration stallBound) {
  const Clock::time_point progressAt = progress != nullptr ? progress->observe() : waitStart;
  return std::max(
      std::chrono::nanoseconds::zero(),
      std::chrono::duration_cast<std::chrono::nanoseconds>(progressAt + stallBound - Clock::now()));
}

/// Waits for \p fences in steps of at most \ref kStepInterval, looking for progress and for a
/// declared loss between steps; see \ref WaitForFencesWhileQueueProgresses.
FenceProgressWait WaitInSteps(const VulkanApi& api, VkDevice device,
                              std::span<const VkFence> fences, VulkanQueueProgress* progress,
                              const DeviceLostState* rootLoss, Clock::duration stallBound,
                              const std::function<void()>& stepHook) {
  const Clock::time_point waitStart = Clock::now();
  for (;;) {
    const std::chrono::nanoseconds remaining = TimeUntilStalled(progress, waitStart, stallBound);
    // A step of zero still asks once, so a fence that has signalled is never reported stalled.
    const std::chrono::nanoseconds step = std::min(remaining, kStepInterval);
    const VkResult result = WaitForAllFences(api, device, fences, step);
    if (result != VK_TIMEOUT) {
      return {EndForAnsweredWait(result), result};
    }
    if (stepHook) {
      stepHook();
    }
    if (RootDeclaredLost(rootLoss)) {
      return {FenceProgressWaitEnd::RootLost, result};
    }
    // When the bound ran out on this step, look once more: progress seen during it moves the
    // deadline, and only a queue that has still not progressed is stalled.
    if (remaining <= step &&
        TimeUntilStalled(progress, waitStart, stallBound) == std::chrono::nanoseconds::zero()) {
      return {FenceProgressWaitEnd::Stalled, result};
    }
  }
}

}  // namespace

FenceProgressWait WaitForFencesWhileQueueProgresses(const VulkanApi& api, VkDevice device,
                                                    std::span<const VkFence> fences,
                                                    VulkanQueueProgress* progress,
                                                    const DeviceLostState* rootLoss,
                                                    std::chrono::steady_clock::duration stallBound,
                                                    const std::function<void()>& stepHook) {
  if (fences.empty()) {
    return {FenceProgressWaitEnd::Signalled, VK_SUCCESS};
  }
  const Clock::duration bound = std::max(stallBound, Clock::duration::zero());
  if (progress != nullptr || rootLoss != nullptr) {
    return WaitInSteps(api, device, fences, progress, rootLoss, bound, stepHook);
  }
  // Nothing to look at between steps, so one wait for the whole bound does the same.
  const VkResult result = WaitForAllFences(api, device, fences, bound);
  return {result == VK_TIMEOUT ? FenceProgressWaitEnd::Stalled : EndForAnsweredWait(result),
          result};
}

}  // namespace donner::gpu::vulkan
