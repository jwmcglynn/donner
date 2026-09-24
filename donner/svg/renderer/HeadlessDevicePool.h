#pragma once
/// @file
/// \c donner::svg::details::HeadlessDevicePool - the small cache of idle headless devices that
/// renderers without a device of their own draw from.

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace donner::svg::details {

/**
 * A bounded cache of idle headless devices, shared by every renderer that does not bring a device
 * of its own, so a renderer torn down and rebuilt reuses a device instead of opening another.
 *
 * A device is handed out as a lease: releasing the last reference to what \ref acquire returned
 * puts the device back, unless it has been lost. A lost device is never handed out again, and a
 * full cache gives up its oldest idle device to keep the one just released.
 *
 * A device bound to the thread that opened it, as a browser device is to its worker, is handed
 * back only to that thread; any other device goes to whichever thread asks. An idle device bound
 * to a thread that has since exited can never be handed out again, which is why a full cache
 * gives up its oldest device rather than the newest.
 *
 * @tparam Device Device type. It provides `bool isDeviceLost() const` and
 *   `bool isBoundToCreatingThread() const`.
 */
template <typename Device>
class HeadlessDevicePool {
public:
  /// Opens a new device, or returns null when none can be opened.
  using Factory = std::function<std::shared_ptr<Device>()>;

  /**
   * Constructs an empty cache.
   *
   * @param create Opens a device when no idle one can be handed out.
   * @param maxIdleDevices Most idle devices the cache keeps.
   */
  HeadlessDevicePool(Factory create, std::size_t maxIdleDevices)
      : create_(std::move(create)), maxIdleDevices_(maxIdleDevices) {}

  HeadlessDevicePool(const HeadlessDevicePool&) = delete;
  HeadlessDevicePool& operator=(const HeadlessDevicePool&) = delete;

  /**
   * Hands out an idle device that is not lost, or a new one.
   *
   * Every lease keeps a pointer to this cache, so the cache must outlive them.
   *
   * @return A lease on the device, or null when no device could be opened or the new one is
   *   already lost.
   */
  std::shared_ptr<Device> acquire() {
    const std::thread::id here = std::this_thread::get_id();
    Idle taken;
    for (;;) {
      {
        const std::lock_guard lock(mutex_);
        // The most recently released device this thread may use.
        auto usable = idle_.end();
        for (auto it = idle_.begin(); it != idle_.end(); ++it) {
          if (!it->device->isBoundToCreatingThread() || it->creator == here) {
            usable = it;
          }
        }
        if (usable == idle_.end()) {
          break;
        }
        taken = std::move(*usable);
        idle_.erase(usable);
      }
      if (!taken.device->isDeviceLost()) {
        break;
      }
      taken = Idle{};
    }
    if (!taken.device) {
      taken = Idle{create_(), here};
    }
    if (!taken.device || taken.device->isDeviceLost()) {
      return nullptr;
    }

    auto lease = std::make_shared<Lease>(this, std::move(taken));
    return std::shared_ptr<Device>(lease, lease->idle.device.get());
  }

  /// Number of idle devices the cache holds.
  [[nodiscard]] std::size_t idleCount() const {
    const std::lock_guard lock(mutex_);
    return idle_.size();
  }

private:
  /// A device and the thread that opened it.
  struct Idle {
    std::shared_ptr<Device> device;  //!< The device.
    std::thread::id creator;         //!< Thread that opened it.
  };

  /// What a handed-out device's references share: the device, returned to the cache when the
  /// last reference goes.
  struct Lease {
    /// @param pool Cache to return the device to. @param idle Device handed out.
    Lease(HeadlessDevicePool* pool, Idle idle) : pool(pool), idle(std::move(idle)) {}
    ~Lease() { pool->release(std::move(idle)); }

    HeadlessDevicePool* pool;  //!< Cache the device returns to.
    Idle idle;                 //!< Device handed out.
  };

  /// Keeps \p released for a later \ref acquire unless it is lost, giving up the oldest idle
  /// device when the cache is full. A device given up is destroyed here, outside the lock.
  /// @param released Device a lease released.
  void release(Idle released) {
    if (!released.device || released.device->isDeviceLost() || maxIdleDevices_ == 0) {
      return;
    }
    Idle givenUp;
    {
      const std::lock_guard lock(mutex_);
      if (idle_.size() >= maxIdleDevices_) {
        givenUp = std::move(idle_.front());
        idle_.erase(idle_.begin());
      }
      idle_.push_back(std::move(released));
    }
  }

  Factory create_;
  std::size_t maxIdleDevices_;
  mutable std::mutex mutex_;
  std::vector<Idle> idle_;  //!< Idle devices, oldest first.
};

}  // namespace donner::svg::details
