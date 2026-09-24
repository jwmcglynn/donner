#pragma once
/// @file
/// \c donner::svg::details::HeadlessDevicePool - the small cache of idle headless devices that
/// renderers without a device of their own draw from.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace donner::svg::details {

/**
 * A bounded cache of idle headless devices, shared by every renderer that does not bring a device
 * of its own, so a renderer torn down and rebuilt reuses a device instead of opening another.
 *
 * A device is handed out as a lease: releasing the last reference to what \ref acquire returned
 * puts the device back, unless it has been lost. A lost device is never handed out again.
 *
 * A device bound to the thread that opened it, as a browser device is to its worker, is handed
 * back only to that thread, and only that thread may destroy it: a browser device destroyed
 * elsewhere cannot release what it made. Any other device goes to, and may be destroyed by,
 * whichever thread asks. A full cache therefore gives up the oldest idle device the releasing
 * thread may destroy, which is the device just released when every older one belongs to another
 * thread. An idle device bound to a thread that has since exited can never be handed out again,
 * which is why the oldest goes rather than the newest. The cache holds more than its bound only
 * while every idle device belongs to a thread other than the one releasing, which that release
 * cannot destroy.
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
    const uint64_t here = ThisThreadToken();
    Idle taken;
    for (;;) {
      {
        const std::lock_guard lock(mutex_);
        // The most recently released device this thread may use.
        auto usable = idle_.end();
        for (auto it = idle_.begin(); it != idle_.end(); ++it) {
          if (it->usableFrom(here)) {
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
  /// A number naming the calling thread, never given to another thread in this process. A thread
  /// identifier would do only while its thread lives: one that has exited can be reused, which
  /// would hand the exited thread's bound devices to a new thread.
  static uint64_t ThisThreadToken() {
    static std::atomic<uint64_t> nextToken{1};
    thread_local const uint64_t token = nextToken.fetch_add(1, std::memory_order_relaxed);
    return token;
  }

  /// A device and the thread that opened it.
  struct Idle {
    std::shared_ptr<Device> device;  //!< The device.
    uint64_t creator = 0;            //!< \ref ThisThreadToken of the thread that opened it.

    /// Whether the thread named \p thread may take the device, or destroy it.
    /// @param thread \ref ThisThreadToken of the asking thread.
    [[nodiscard]] bool usableFrom(uint64_t thread) const {
      return !device->isBoundToCreatingThread() || creator == thread;
    }
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

  /// Keeps \p released for a later \ref acquire unless it is lost. When the cache is full it gives
  /// up the oldest idle device this thread may destroy, which may be \p released itself, and
  /// destroys it here, outside the lock.
  /// @param released Device a lease released.
  void release(Idle released) {
    if (!released.device || released.device->isDeviceLost()) {
      return;
    }
    const uint64_t here = ThisThreadToken();
    Idle givenUp;
    {
      const std::lock_guard lock(mutex_);
      idle_.push_back(std::move(released));
      if (idle_.size() > maxIdleDevices_) {
        for (auto it = idle_.begin(); it != idle_.end(); ++it) {
          if (it->usableFrom(here)) {
            givenUp = std::move(*it);
            idle_.erase(it);
            break;
          }
        }
      }
    }
  }

  Factory create_;
  std::size_t maxIdleDevices_;
  mutable std::mutex mutex_;
  std::vector<Idle> idle_;  //!< Idle devices, oldest first.
};

}  // namespace donner::svg::details
