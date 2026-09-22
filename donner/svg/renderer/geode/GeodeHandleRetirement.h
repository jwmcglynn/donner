#pragma once
/// @file
/// \c donner::geode::GeodeHandleRetirement - handles let go of on another thread, released on
/// their own device's thread.

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include "donner/gpu/Handles.h"

namespace donner::geode {

/**
 * Buffers and bind groups of one runtime device that were let go of where that device may not be
 * used, kept until the context rendering through the device releases them on its own thread.
 *
 * A runtime device is single-threaded: releasing a handle returns its slot to the device's tables,
 * which the device's own thread may be using at the same moment. State a document keeps for a
 * device can be destroyed on whichever thread holds the document at the time - another device's
 * render worker, or the thread destroying the document - so it hands its handles here instead of
 * dropping them.
 *
 * Retiring is thread-safe and always takes the handles. Only the context releases, on its own
 * thread. Closing, at the context's teardown, releases what is held one last time; handles retired
 * after that stay here until this object is destroyed.
 *
 * Whoever owns the runtime device must keep this object alive until the device is gone, and no
 * longer than it needs to: the handles still held when this object is destroyed are dropped then,
 * and a handle whose device is gone releases nothing (see `donner/gpu/Handles.h`). That is what
 * keeps a late retirement from releasing into a device another thread is tearing down.
 */
class GeodeHandleRetirement {
public:
  GeodeHandleRetirement() = default;
  GeodeHandleRetirement(const GeodeHandleRetirement&) = delete;
  GeodeHandleRetirement& operator=(const GeodeHandleRetirement&) = delete;

  /**
   * Takes every handle in \p buffers and \p bindGroups, leaving both empty.
   *
   * @param buffers Buffers to retire.
   * @param bindGroups Bind groups to retire.
   */
  void retire(std::vector<gpu::Buffer>& buffers, std::vector<gpu::BindGroup>& bindGroups);

  /// Releases everything held. Call on the thread of the context rendering through the device.
  void release();

  /// Releases everything held, for the last time, and marks the context gone. Call from the
  /// context's teardown, while its runtime device still exists.
  void close();

  /// Whether the context has closed this retirement: nothing renders through its device any more,
  /// so state kept for that device can be dropped.
  [[nodiscard]] bool closed() const { return closed_.load(std::memory_order_acquire); }

  /// Handles waiting to be released. Test accessor.
  [[nodiscard]] size_t heldCountForTesting() const;

private:
  mutable std::mutex mutex_;
  std::atomic<bool> closed_ = false;
  std::vector<gpu::Buffer> buffers_;
  std::vector<gpu::BindGroup> bindGroups_;
};

}  // namespace donner::geode
