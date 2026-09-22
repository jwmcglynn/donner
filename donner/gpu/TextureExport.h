#pragma once
/// @file
/// \c donner::gpu::TextureExport - naming a texture of one runtime device on another.
///
/// Two runtime devices over one backend device keep separate handle tables, serials and
/// retirement, and they are often driven from different threads and may submit to different native
/// queues. A texture of one is not a texture of the other until it is registered there. The
/// producer exports the texture on its own thread, which is the only thread allowed to read its
/// table; the consumer registers the export on its own thread and never touches the producer
/// device. What crosses between the two is the token declared here.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/DeviceLost.h"

namespace donner::gpu {

class Device;

/// Which backend a device belongs to and which native device it records against. Two devices can
/// name each other's textures only when both fields match. The pointers are compared, never
/// dereferenced.
struct BackendDeviceIdentity {
  const void* family = nullptr;        //!< Address of a per-backend static tag.
  const void* nativeDevice = nullptr;  //!< Native device object the backend records against.

  /// Whether both fields name something; a backend that cannot share textures reports neither.
  [[nodiscard]] bool isValid() const { return family != nullptr && nativeDevice != nullptr; }

  /// Equality operator. @param other Identity to compare against.
  bool operator==(const BackendDeviceIdentity& other) const = default;
};

/**
 * Backend half of an exported texture: a reference to the native allocation, plus whatever a
 * sibling device of the same backend needs to name it.
 *
 * Every export token and registration made from one export shares this object, so the allocation
 * lives exactly as long as the last of them. The last reference can be dropped on any thread and
 * after either device is gone, so a derived class's destructor must be safe anywhere and must
 * retain whatever releasing the native object requires.
 */
class ExportedTextureBacking {
public:
  ExportedTextureBacking() = default;
  virtual ~ExportedTextureBacking();

  ExportedTextureBacking(const ExportedTextureBacking&) = delete;
  ExportedTextureBacking& operator=(const ExportedTextureBacking&) = delete;

  /**
   * Releases the allocation at once, for a producer that asked to give its backing back while
   * another device still read it. Called exactly once, by whichever thread drops the last holder,
   * after the producer has released its handle; nothing names the texture afterwards.
   *
   * The default does nothing, which is right for a backend whose allocation goes away with its
   * last reference.
   */
  virtual void releaseBackingNow() const;
};

/// How work a consumer submits against a registration is ordered after the producer's work.
enum class SourceOrdering : uint8_t {
  /// Producer and consumer hand their work to one native queue, so submission order is execution
  /// order and nothing has to wait.
  SharedQueue,
  /// Producer and consumer submit to different native queues. Consumer work that names the
  /// registration may reach the queue only once the producer work it is ordered after has
  /// completed.
  WaitForSource,
};

/**
 * Ostream output operator for \ref SourceOrdering, e.g. `SharedQueue`.
 *
 * @param os Output stream.
 * @param value Value to output.
 */
std::ostream& operator<<(std::ostream& os, SourceOrdering value);

/// Thread-safe view of a producer's submission completion, read by consumers on their own threads.
class SubmissionCompletion {
public:
  virtual ~SubmissionCompletion();

  /// Highest producer submission serial known to have completed. Callable from any thread.
  [[nodiscard]] virtual uint64_t completedSerial() const = 0;

  /// Whether the producer's backend has reported a terminal execution failure, after which no
  /// serial it reports as complete can be trusted. Callable from any thread.
  [[nodiscard]] virtual bool failed() const = 0;
};

/// What a backend reports when it exports one of its textures.
struct BackendTextureExport {
  /// The native allocation, retained. Never null on success.
  std::shared_ptr<const ExportedTextureBacking> backing;
  /// How registrations of this texture are ordered after the producer's work.
  SourceOrdering ordering = SourceOrdering::WaitForSource;
  /// The producer's completion. Required for \ref SourceOrdering::WaitForSource.
  std::shared_ptr<const SubmissionCompletion> completion;
  /// Producer serial whose completion makes the texture's current contents final, counting work
  /// the backend performed outside the runtime's own record of submissions (queued uploads).
  uint64_t contentSerial = 0;
  /// Whether a write to the texture is queued for the producer's next submission and has not been
  /// carried by one yet.
  bool writePending = false;
};

namespace details {

/**
 * Shared state of one exported texture of a producer device.
 *
 * Created by the producer's first export of the texture and kept by the producer's table until it
 * releases its handle. The producer updates the content serial on its own thread; consumers read
 * it on theirs. The holder count and the tail accounting are guarded by one mutex, because they
 * change only when an export or registration comes or goes, never per draw.
 */
class TextureShare {
public:
  /**
   * Records a newly exported texture.
   *
   * @param descriptor The producer's record of the texture.
   * @param producerDeviceId Identity of the producer device.
   * @param identity Backend and native device the producer records against.
   * @param producerLostState The producer's loss condition.
   * @param backend What the producer's backend reported for the texture.
   * @param tailBytes The producer's gauge of bytes still held elsewhere after it released them.
   */
  TextureShare(TextureDescriptor descriptor, uint64_t producerDeviceId,
               BackendDeviceIdentity identity, std::shared_ptr<DeviceLostState> producerLostState,
               BackendTextureExport backend, std::shared_ptr<std::atomic<uint64_t>> tailBytes);

  TextureShare(const TextureShare&) = delete;
  TextureShare& operator=(const TextureShare&) = delete;

  /// The producer's record of the texture.
  const TextureDescriptor& descriptor() const UTILS_LIFETIME_BOUND { return descriptor_; }
  /// Identity of the producer device.
  uint64_t producerDeviceId() const { return producerDeviceId_; }
  /// Backend and native device the producer records against.
  const BackendDeviceIdentity& identity() const UTILS_LIFETIME_BOUND { return identity_; }
  /// The native allocation the producer's backend exported.
  const ExportedTextureBacking& backing() const UTILS_LIFETIME_BOUND { return *backend_.backing; }
  /// How registrations are ordered after the producer's work.
  SourceOrdering ordering() const { return backend_.ordering; }
  /// The producer's completion, or null for \ref SourceOrdering::SharedQueue.
  const SubmissionCompletion* completion() const { return backend_.completion.get(); }
  /// The producer's loss condition.
  DeviceLostState& producerLostState() const { return *producerLostState_; }
  /// Whether the producer's loss condition is set.
  bool producerLost() const { return producerLostState_->lost.load(std::memory_order_acquire); }

  /// Producer serial whose completion makes the current contents final.
  uint64_t contentSerial() const { return contentSerial_.load(std::memory_order_acquire); }
  /// Whether a producer write is queued and not yet carried by a submission.
  bool writePending() const { return writePending_.load(std::memory_order_acquire); }

  /// Producer thread: a submission accepted with \p serial referenced the texture.
  /// @param serial Accepted submission serial.
  void noteProducerUse(uint64_t serial);
  /// Producer thread: a write to the texture is queued for the next submission.
  void noteWritePending();
  /// Producer thread: the submission accepted with \p serial carried the queued writes.
  /// @param serial Accepted submission serial.
  void noteWritesCarried(uint64_t serial);

  /// A token or registration started holding the allocation.
  void acquire();
  /// A token or registration stopped holding the allocation.
  void release();
  /// The producer released its handle; the allocation now lives only as long as other holders.
  void releaseProducer();
  /// The producer asked to release the allocation at once while another holder still had it;
  /// the release happens when the last holder lets go.
  void requestBackingRelease();

  /// Whether the producer has released its handle.
  bool producerReleased() const;
  /// Whether any token or registration still holds the allocation.
  bool heldElsewhere() const;

private:
  /// Adds or removes this texture's bytes from the tail gauge when the tail condition changed,
  /// and releases the allocation once a requested release has no holder left to wait for.
  /// Requires \ref mutex_.
  void updateTailLocked();

  const TextureDescriptor descriptor_;
  const uint64_t producerDeviceId_;
  const BackendDeviceIdentity identity_;
  const std::shared_ptr<DeviceLostState> producerLostState_;
  const BackendTextureExport backend_;
  const std::shared_ptr<std::atomic<uint64_t>> tailBytes_;
  const uint64_t allocationBytes_;

  std::atomic<uint64_t> contentSerial_{0};
  std::atomic<bool> writePending_{false};

  mutable std::mutex mutex_;
  uint32_t holders_ = 0;           //!< Guarded by mutex_.
  bool producerReleased_ = false;  //!< Guarded by mutex_.
  bool countedInTail_ = false;     //!< Guarded by mutex_.
  bool releaseRequested_ = false;  //!< Guarded by mutex_.
  bool backingReleased_ = false;   //!< Guarded by mutex_.
};

/// One holder of a share: every copy of one export token shares one lease, and each registration
/// holds its own. Constructing it counts a holder and destroying it releases that holder.
class TextureShareLease {
public:
  /// Counts a holder of \p share. @param share Share to hold; must not be null.
  explicit TextureShareLease(std::shared_ptr<TextureShare> share);
  /// Releases the holder.
  ~TextureShareLease();

  TextureShareLease(const TextureShareLease&) = delete;
  TextureShareLease& operator=(const TextureShareLease&) = delete;

  /// The held share.
  const std::shared_ptr<TextureShare>& share() const UTILS_LIFETIME_BOUND { return share_; }

private:
  std::shared_ptr<TextureShare> share_;
};

}  // namespace details

/**
 * An immutable, thread-safe token naming one texture of a producer device, for registration on
 * another runtime device over the same backend device.
 *
 * Produced by \ref Device::exportTexture on the producer's thread and consumed by
 * \ref Device::registerTexture on the consumer's. Copies share one holder of the allocation: the
 * texture's memory stays alive while any copy, or any registration made from one, is alive, even
 * after the producer has released its handle. An empty token names nothing.
 */
class TextureExport {
public:
  /// Constructs an empty token.
  TextureExport() = default;

  /// Whether the token names a texture.
  [[nodiscard]] bool isValid() const { return lease_ != nullptr; }

  /// The producer's record of the texture. Requires \ref isValid.
  [[nodiscard]] const TextureDescriptor& descriptor() const UTILS_LIFETIME_BOUND;

  /// Identity of the device that exported the texture, or zero for an empty token.
  [[nodiscard]] uint64_t producerDeviceId() const;

private:
  friend class Device;

  /// Wraps a holder of \p lease's share. @param lease Holder created for this export.
  explicit TextureExport(std::shared_ptr<const details::TextureShareLease> lease)
      : lease_(std::move(lease)) {}

  std::shared_ptr<const details::TextureShareLease> lease_;
};

}  // namespace donner::gpu
