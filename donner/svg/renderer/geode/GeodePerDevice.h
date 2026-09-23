#pragma once
/// @file
/// \c donner::geode::GeodePerDevice - one value per device that draws a document.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"

namespace donner::geode {

/// Identifies a device to state kept per device: its context's process-unique id, and the
/// retirement that context releases handles through, which says whether the context still exists.
struct GeodeDeviceKey {
  uint64_t deviceId = 0;                                    //!< `GeodeDevice::deviceId()`.
  std::shared_ptr<GeodeHandleRetirement> handleRetirement;  //!< The context's retirement.
};

/**
 * One \p T per device that draws a document, kept on the document.
 *
 * A document can be drawn by renderers on several devices - a render worker's and a UI thread's
 * thumbnail pass, say. State one device builds for the document holds that device's handles, so a
 * second device must neither reuse it nor replace it: replacing it would destroy the first
 * device's state on the second device's thread and make the first device rebuild it on its next
 * frame. Each device gets its own entry instead. An entry whose context has gone away (its
 * retirement is closed or destroyed) stays until \ref dropGone drops it; looking entries up never
 * drops one, so the owner decides where that happens and can do everything that goes with it
 * there. Dropping is safe on any thread: the entry's state hands its handles to a closed
 * retirement, which keeps them, or, once that retirement is destroyed, drops them after their
 * device is gone, when releasing them does nothing.
 *
 * A document drops gone devices' entries in one place: when its own entry for a gone device is
 * dropped, it drops that device's entries from every per-entity component that holds one of
 * these. A new per-entity component type holding one must be added to that sweep (see
 * `DocumentPerDeviceComponents` in RendererGeode.cc), or a gone device's slots would keep its slabs
 * alive.
 *
 * Values are held by pointer, so a reference to one stays valid while other devices' entries come
 * and go.
 *
 * @tparam T Per-device state; default-constructible.
 */
template <typename T>
class GeodePerDevice {
public:
  GeodePerDevice() = default;
  ~GeodePerDevice() = default;
  // Declared, not left implicit: a vector of move-only entries still reports itself copyable, and
  // the registry copies a component that reports itself copyable.
  GeodePerDevice(const GeodePerDevice&) = delete;
  GeodePerDevice& operator=(const GeodePerDevice&) = delete;
  GeodePerDevice(GeodePerDevice&&) noexcept = default;
  GeodePerDevice& operator=(GeodePerDevice&&) noexcept = default;

  /// The entry for \p device, creating an empty one when it has none. Drops nothing.
  /// @param device Device the entry belongs to.
  T& forDevice(const GeodeDeviceKey& device) {
    for (Entry& entry : entries_) {
      if (entry.deviceId == device.deviceId) {
        return *entry.value;
      }
    }
    entries_.push_back(Entry{device.deviceId, device.handleRetirement, std::make_unique<T>()});
    return *entries_.back().value;
  }

  /// Drops the entries of contexts that are gone.
  /// @return Number of entries dropped.
  size_t dropGone() {
    return std::erase_if(entries_, [](const Entry& entry) { return ContextGone(entry); });
  }

  /// The entry for the device \p deviceId, or null when it has none.
  /// @param deviceId `GeodeDevice::deviceId()` of the device.
  T* find(uint64_t deviceId) {
    for (Entry& entry : entries_) {
      if (entry.deviceId == deviceId) {
        return entry.value.get();
      }
    }
    return nullptr;
  }

  /// Calls \p fn with every device's entry.
  /// @param fn Callable taking `T&`.
  template <typename Fn>
  void forEach(Fn&& fn) {
    for (Entry& entry : entries_) {
      fn(*entry.value);
    }
  }

  /// Number of devices with an entry.
  size_t size() const { return entries_.size(); }

private:
  struct Entry {
    uint64_t deviceId = 0;
    std::weak_ptr<GeodeHandleRetirement> retirement;
    std::unique_ptr<T> value;
  };
  static_assert(!std::is_copy_constructible_v<Entry> &&
                    std::is_nothrow_move_constructible_v<Entry> &&
                    std::is_nothrow_move_assignable_v<Entry>,
                "an entry owns its value and moves without throwing inside the vector");

  static bool ContextGone(const Entry& entry) {
    const std::shared_ptr<GeodeHandleRetirement> retirement = entry.retirement.lock();
    return retirement == nullptr || retirement->closed();
  }

  std::vector<Entry> entries_;
};

}  // namespace donner::geode
