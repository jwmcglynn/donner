#pragma once
/// @file
/// \c donner::gpu::browser::BrowserShareReleaseQueue - texture share releases waiting for the
/// worker that owns them.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

/**
 * Texture share releases waiting for the worker that made the shares.
 *
 * The last holder of a share may be let go on any thread, because an export token may be, but only
 * the worker that made a share can tell its browser side to let the texture go. A holder let go on
 * another thread posts its release here instead, and the owner runs what is posted the next time it
 * drains the queue. A queue whose owner has gone is closed: a post is then refused and does
 * nothing, and whatever was still waiting is dropped with the worker's browser state.
 *
 * Any thread may post; only the owner drains or closes.
 */
class BrowserShareReleaseQueue {
public:
  /// A release to run on the owner.
  struct Release {
    uint32_t producer = 0;            //!< Logical device that made the share.
    BrowserTextureShareId share = 0;  //!< Share to release.
  };

  /**
   * Queues \p release for the owner, unless the queue is closed.
   *
   * @param release Release to queue.
   * @param notify Run, while the owner cannot close the queue, once the release is queued, so a
   *   caller can wake the owner knowing it has not gone; not run when the queue is closed. May be
   *   empty.
   * @return Whether the release was queued.
   */
  bool post(const Release& release, const std::function<void()>& notify = {});

  /// Takes every queued release, oldest first. Owner only.
  std::vector<Release> takeAll();

  /// Refuses every later post and drops what is still queued, because the owner is going. Owner
  /// only; waits for a post in progress to finish.
  void close();

  /// Whether \ref close has run.
  [[nodiscard]] bool closed() const;

private:
  mutable std::mutex mutex_;
  bool closed_ = false;
  std::vector<Release> pending_;
};

}  // namespace donner::gpu::browser
