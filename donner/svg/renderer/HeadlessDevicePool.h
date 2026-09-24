#pragma once
/// @file
/// \c donner::svg::details::HeadlessDevicePool - the small cache of idle headless devices that
/// renderers without a device of their own draw from.

#include <cstddef>
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
 * puts the device back, unless it has been lost or the cache is full. A lost device is never
 * handed out again.
 *
 * @tparam Device Device type. It provides `bool isDeviceLost() const`.
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
    std::shared_ptr<Device> device;
    for (;;) {
      {
        const std::lock_guard lock(mutex_);
        if (idle_.empty()) {
          break;
        }
        device = std::move(idle_.back());
        idle_.pop_back();
      }
      if (device && !device->isDeviceLost()) {
        break;
      }
      device.reset();
    }
    if (!device) {
      device = create_();
    }
    if (!device || device->isDeviceLost()) {
      return nullptr;
    }

    auto lease = std::make_shared<Lease>(this, std::move(device));
    return std::shared_ptr<Device>(lease, lease->device.get());
  }

  /// Number of idle devices the cache holds.
  [[nodiscard]] std::size_t idleCount() const {
    const std::lock_guard lock(mutex_);
    return idle_.size();
  }

private:
  /// What a handed-out device's references share: the device, returned to the cache when the
  /// last reference goes.
  struct Lease {
    /// @param pool Cache to return the device to. @param device Device handed out.
    Lease(HeadlessDevicePool* pool, std::shared_ptr<Device> device)
        : pool(pool), device(std::move(device)) {}
    ~Lease() { pool->release(std::move(device)); }

    HeadlessDevicePool* pool;        //!< Cache the device returns to.
    std::shared_ptr<Device> device;  //!< Device handed out.
  };

  /// Keeps \p device for a later \ref acquire, unless it is lost or the cache is full.
  /// @param device Device a lease released.
  void release(std::shared_ptr<Device> device) {
    if (!device || device->isDeviceLost()) {
      return;
    }
    const std::lock_guard lock(mutex_);
    if (idle_.size() < maxIdleDevices_) {
      idle_.push_back(std::move(device));
    }
  }

  Factory create_;
  std::size_t maxIdleDevices_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Device>> idle_;
};

}  // namespace donner::svg::details
