#pragma once
/// @file
/// \c donner::geode::GeodeHandleRetirement - handles let go of on another thread, released on
/// their own device's thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "donner/gpu/Handles.h"

namespace donner::geode {

/**
 * Buffers, bind groups and textures of one runtime device that were let go of where that device
 * may not be used, kept until the context rendering through the device releases them on its own
 * thread.
 *
 * A runtime device is single-threaded: releasing a handle returns its slot to the device's tables,
 * which the device's own thread may be using at the same moment. State a document keeps for a
 * device can be destroyed on whichever thread holds the document at the time - another device's
 * render worker, or the thread destroying the document - so it hands its handles here instead of
 * dropping them.
 *
 * Retiring is thread-safe and takes only handles of this retirement's device. Only the context
 * releases, on its own thread. Closing, at the context's teardown, releases what is held and marks
 * the context gone. Handles retired after that stay here until the context's own teardown releases
 * them, while it still can, or until this object is destroyed.
 *
 * Whoever owns the runtime device must keep this object alive until the device is gone, and no
 * longer than it needs to: the handles still held when this object is destroyed are dropped then,
 * and a handle whose device is gone releases nothing (see `donner/gpu/Handles.h`). That is what
 * keeps a late retirement from releasing into a device another thread is tearing down.
 *
 * For the first context over a root, that owner is the root's physical-device owner, which keeps
 * the root device alive for later contexts. So when that context closes while others still render
 * through the root, handles retired to it afterwards keep their GPU memory until the owner goes,
 * although nothing renders through the root device any more. That is bounded by what the closed
 * context built.
 */
class GeodeHandleRetirement {
public:
  /// Handles waiting to be released, by kind. Test accessor.
  struct HeldCounts {
    std::size_t buffers = 0;     //!< Buffers held.
    std::size_t bindGroups = 0;  //!< Bind groups held.
    std::size_t textures = 0;    //!< Textures held.
  };

  /// @param runtimeDeviceId `gpu::Device::deviceId()` of the runtime device whose handles this
  ///   retirement takes.
  explicit GeodeHandleRetirement(uint64_t runtimeDeviceId) : runtimeDeviceId_(runtimeDeviceId) {}
  GeodeHandleRetirement(const GeodeHandleRetirement&) = delete;
  GeodeHandleRetirement& operator=(const GeodeHandleRetirement&) = delete;

  /**
   * Takes every handle of this retirement's device in \p buffers and \p bindGroups, leaving the
   * others where they are, and removes null handles.
   *
   * @param buffers Buffers to retire.
   * @param bindGroups Bind groups to retire.
   * @return Number of handles left with the caller because another device owns them.
   */
  [[nodiscard]] std::size_t retire(std::vector<gpu::Buffer>& buffers,
                                   std::vector<gpu::BindGroup>& bindGroups);

  /**
   * Takes every texture of this retirement's device in \p textures, leaving the others where they
   * are, and removes null handles.
   *
   * @param textures Textures to retire.
   * @return Number of textures left with the caller because another device owns them.
   */
  [[nodiscard]] std::size_t retireTextures(std::vector<gpu::Texture>& textures);

  /// Called on the releasing thread with each held texture's identity just before \ref release
  /// lets go of it.
  using TextureReleaseListener = std::function<void(const gpu::ResourceIdentity& texture)>;

  /**
   * Have \p listener told of every texture \ref release lets go of, before it does, so state that
   * holds views of the texture can drop them first. There is one listener: the device's one live
   * texture pool. A new listener may replace an earlier one only once that one reports itself gone
   * (see \p isLive); replacing a live one is a programming error, asserted in debug builds. The
   * listener runs on the thread calling \ref release, without this object's lock, and must not
   * call back into this object.
   *
   * @param listener Callback receiving each released texture's identity.
   * @param isLive Whether \p listener's owner still exists; consulted when a later call replaces
   * it.
   */
  void setTextureReleaseListener(TextureReleaseListener listener, std::function<bool()> isLive);

  /// Wake the owning event loop when another thread hands it work. The callback runs under this
  /// mailbox's lock and must only post a nonblocking wake; clearing it waits for any call to end.
  void setWakeCallback(std::function<void()> callback);

  /// Whether an owner-thread idle pass has handles to release.
  [[nodiscard]] bool hasPending() const;

  /// Releases everything held. Call on the thread of the context rendering through the device.
  void release();

  /// Releases everything held and marks the context gone. Call from the context's teardown, while
  /// its runtime device still exists.
  void close();

  /// Whether the context has closed this retirement: nothing renders through its device any more,
  /// so state kept for that device can be dropped.
  [[nodiscard]] bool closed() const { return closed_.load(std::memory_order_acquire); }

  /// `gpu::Device::deviceId()` of the runtime device whose handles this retirement takes.
  [[nodiscard]] uint64_t runtimeDeviceId() const { return runtimeDeviceId_; }

  /// Handles waiting to be released. Test accessor.
  [[nodiscard]] HeldCounts heldCountsForTesting() const;

private:
  const uint64_t runtimeDeviceId_;
  mutable std::mutex mutex_;
  std::atomic<bool> closed_ = false;
  std::vector<gpu::Buffer> buffers_;
  std::vector<gpu::BindGroup> bindGroups_;
  std::vector<gpu::Texture> textures_;
  TextureReleaseListener textureReleaseListener_;
  std::function<bool()> textureReleaseListenerLive_;
  std::function<void()> wakeCallback_;
};

}  // namespace donner::geode
