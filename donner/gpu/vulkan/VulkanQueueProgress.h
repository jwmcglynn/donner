#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanQueueProgress - when the one queue every runtime device over a
/// Vulkan root submits to last finished work, and fence waits that give up only when it stops.

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ostream>
#include <span>
#include <vector>

#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan {

/**
 * When the one queue every runtime device over a Vulkan root submits to last made progress.
 *
 * Work on that queue completes in order, so a submission completes only after everything queued
 * ahead of it, other devices' work included. A wait for it therefore judges a hang by whether the
 * queue is still finishing work, not by a budget for the whole backlog, and not by its own
 * device's completions alone, which a sibling's long work ahead of it would hold back.
 *
 * Each command buffer a runtime device submits ends by setting an event taken from here, and an
 * event seen set is a command buffer finished. The queue last made progress when an event was
 * last seen set, or when work was submitted while none of the tracked work was outstanding,
 * which starts the clock. That includes work that sets no event, such as a texture upload or a
 * swapchain's frame handover, so a wait for it is not judged by progress from long before it was
 * submitted. A single command buffer that runs longer than a wait's bound is therefore
 * indistinguishable from a hang. Work that sets no event never counts as outstanding, though:
 * each submission made while only such work is outstanding starts the clock again, so a hang in
 * it is caught up to one bound after the last such submission rather than after the hang began.
 *
 * Progress is timed when it is seen, not when it happened: an event is seen when a wait or
 * \ref observe looks, or when its submission completes and its device gives it back. Command
 * buffers that finished while nothing looked therefore count as progress at the next look, and
 * the first wait after such a stretch can run for up to its whole bound from that look even when
 * the queue stopped earlier. A wait that keeps looking sees each one within a step.
 *
 * Events are pooled. One goes back to the pool once the submission whose command buffer set it
 * has completed, and every event is destroyed with this object, which the root destroys before
 * its device. Thread-safe: each device over the root reaches it from its own thread.
 */
class VulkanQueueProgress {
public:
  /**
   * @param api Entry points of the root's device; outlives this object.
   * @param device The root's device; outlives this object.
   */
  VulkanQueueProgress(const VulkanApi& api, VkDevice device);

  /// Destroys every event. No submission that sets one may still be pending.
  ~VulkanQueueProgress();

  VulkanQueueProgress(const VulkanQueueProgress&) = delete;
  VulkanQueueProgress& operator=(const VulkanQueueProgress&) = delete;
  VulkanQueueProgress(VulkanQueueProgress&&) = delete;
  VulkanQueueProgress& operator=(VulkanQueueProgress&&) = delete;

  /// An unset event for a command buffer being encoded to set as it finishes.
  Result<VkEvent> acquire();

  /// Takes back events whose command buffers the queue did not accept, or ran only before a
  /// reported device loss was drained.
  /// @param events Events from \ref acquire that no pending command buffer refers to.
  void releaseUnsubmitted(std::span<const VkEvent> events);

  /// Records that the queue accepted work whose command buffers set \p events, which may be none.
  /// Work accepted while none of the tracked work was outstanding starts the progress clock,
  /// whether or not it sets events. Call under the queue lock, so work is noted in queue order.
  /// @param events Events the accepted command buffers set, from \ref acquire.
  void noteSubmitted(std::span<const VkEvent> events);

  /// Takes back the events of a submission that has completed. Any not yet seen set count as
  /// progress now.
  /// @param events Events the completed submission's command buffers set.
  void retire(std::span<const VkEvent> events);

  /// Looks for events set since the last look, and returns when the queue last made progress.
  std::chrono::steady_clock::time_point observe();

private:
  /// Records the current time as the queue's last progress. Requires \ref mutex_.
  void noteProgressLocked();

  const VulkanApi& api_;          //!< Entry points of the root's device.
  VkDevice device_;               //!< The root's device.
  std::mutex mutex_;              //!< Guards everything below, and host access to every event.
  std::vector<VkEvent> free_;     //!< Unset events ready for another command buffer.
  std::vector<VkEvent> pending_;  //!< Submitted events not yet seen set.
  std::chrono::steady_clock::time_point lastProgress_;  //!< When the queue last made progress.
};

/// How \ref WaitForFencesWhileQueueProgresses ended.
enum class FenceProgressWaitEnd : uint8_t {
  Signalled,  //!< Every fence signalled.
  Stalled,    //!< The queue made no progress for the bound.
  RootLost,   //!< A device over the root declared the root lost first.
  Failed,     //!< The wait itself failed; \ref FenceProgressWait::result says how.
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, FenceProgressWaitEnd value);

/// How a fence wait bounded by queue progress ended, with the native result of its last step.
struct FenceProgressWait {
  FenceProgressWaitEnd end = FenceProgressWaitEnd::Stalled;  //!< How the wait ended.
  VkResult result = VK_TIMEOUT;                              //!< Native result of the last step.
};

/**
 * Waits for every one of \p fences to signal, giving up once the queue they are on has made no
 * progress for \p stallBound.
 *
 * The bound runs from the queue's last progress, which can predate the wait, so a new wait on a
 * queue that was already seen to stop does not restart it; progress no one had looked for yet is
 * timed by this wait's first look (see \ref VulkanQueueProgress). Without a progress record the
 * whole wait is bounded by \p stallBound instead. Waits in steps of at most 10 ms, looking for
 * progress and for a loss declared over the root between them; with neither to look for, it waits
 * once for the whole bound. Records nothing: each caller decides what its outcome means.
 *
 * @param api Entry points of the fences' device.
 * @param device Device the fences belong to.
 * @param fences Fences to wait for, all of them.
 * @param progress Progress of the queue the fences' work is on, or null when none is tracked.
 * @param rootLoss Loss condition that ends the wait once declared, or null for a wait that has to
 *   prove the fence signalled whatever was declared, as teardown does.
 * @param stallBound Longest the queue may go without progress.
 * @param stepHook Run after every step that ends with the fence unsignalled; test seam.
 */
FenceProgressWait WaitForFencesWhileQueueProgresses(const VulkanApi& api, VkDevice device,
                                                    std::span<const VkFence> fences,
                                                    VulkanQueueProgress* progress,
                                                    const DeviceLostState* rootLoss,
                                                    std::chrono::steady_clock::duration stallBound,
                                                    const std::function<void()>& stepHook = {});

}  // namespace donner::gpu::vulkan
