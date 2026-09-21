#pragma once
/// @file
/// \c donner::gpu::BufferMappingTable - the bookkeeping behind a backend's host buffer mappings.

#include <cstdint>
#include <span>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/GpuResult.h"

namespace donner::gpu {

/**
 * The facts a backend supplies about its own buffers so \ref BufferMappingTable can decide when a
 * mapping is readable and which bytes it names.
 *
 * Only these four answers are backend-specific. When a range is ready, what a stale or invalidated
 * handle does, and which error a caller sees are decisions the table makes for every backend, so
 * two backends cannot drift into two different mapping contracts.
 */
class BufferMappingHost {
public:
  /// Virtual destructor; the table never owns a host.
  virtual ~BufferMappingHost() = default;

  /**
   * Host-visible bytes of a live buffer slot, or an empty span when the slot holds no live
   * mappable allocation. The span aliases the backend's allocation and stays valid only while
   * that allocation does, so the table re-asks rather than caching it.
   *
   * @param bufferSlotIndex Slot of the buffer.
   */
  virtual std::span<const uint8_t> mappableBytes(uint32_t bufferSlotIndex) const = 0;

  /// Highest submission serial the device has completed.
  virtual uint64_t completedSubmissionSerial() const = 0;

  /**
   * Waits at most \p sliceSeconds for \p serial to complete, and reports how it spent the slice.
   *
   * Whether the submission completed is not reported here: the table re-reads readiness
   * afterwards anyway, because a wait hands control to the backend and the mapped buffer can be
   * retired while the slice is blocked. What only this call can say is whether it blocked on a
   * completion signal or rechecked readiness itself, which is the difference the readback
   * statistics carry.
   *
   * @param serial Submission serial to wait for.
   * @param sliceSeconds Longest this call may block.
   */
  virtual MapWaitKind waitForSubmission(uint64_t serial, double sliceSeconds) = 0;

  /// Whether the device has taken a terminal failure, after which no submission can ever complete.
  virtual bool deviceLost() const = 0;
};

/**
 * Tracks the host mappings a backend has open and answers the mapping hooks \ref Device dispatches
 * to it.
 *
 * A mapping names a byte range of one buffer plus the submission that must complete before that
 * range holds the bytes the caller asked for. Holding a mapping is not ownership of the buffer:
 * destroying the buffer invalidates the mapping instead of keeping the allocation alive, so a read
 * afterwards is a reported failure rather than a read of freed memory.
 *
 * A mapping observes the work submitted before it was created. Writes still queued for a later
 * submission are not part of it, because the submission that would carry them does not exist yet
 * and waiting for it would be waiting for something the caller has not asked for.
 */
class BufferMappingTable {
public:
  /// @param host Supplies the backend's buffer facts; must outlive this table.
  explicit BufferMappingTable(BufferMappingHost& host) : host_(host) {}

  /**
   * Opens a mapping over \p byteCount bytes of \p bufferSlotIndex starting at \p offsetBytes.
   *
   * @param mappingSlotIndex Slot the mapping occupies.
   * @param bufferSlotIndex Slot of the buffer being mapped.
   * @param offsetBytes Byte offset of the mapped range.
   * @param byteCount Length of the mapped range.
   * @param readySerial Submission whose completion makes the range hold the bytes the caller
   *   asked for. Zero when the buffer has no outstanding work, which is ready immediately.
   */
  Status begin(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex, uint64_t offsetBytes,
               uint64_t byteCount, uint64_t readySerial);

  /**
   * Waits up to \p sliceSeconds for one mapping, and reports what it found and how it waited.
   *
   * @param mappingSlotIndex Slot of the mapping.
   * @param sliceSeconds Longest this call may block.
   */
  MapSliceReport waitSlice(uint32_t mappingSlotIndex, double sliceSeconds);

  /**
   * Bytes of a completed mapping, or a failure while it is pending, released, or invalidated.
   *
   * The span aliases the backend's own allocation rather than a copy of it, so it is valid only
   * until whichever of these comes first: the mapping is released, its buffer is retired, or the
   * device is lost. Callers that outlive any of those copy the bytes out. The annotation states
   * that contract to the compiler rather than enforcing it: the span is returned inside a
   * \ref Result, and the dangling diagnostic does not see through an unannotated class template.
   *
   * @param mappingSlotIndex Slot of the mapping.
   */
  Result<std::span<const uint8_t>> bytes(uint32_t mappingSlotIndex) const UTILS_LIFETIME_BOUND;

  /**
   * Releases a mapping, so every remaining handle naming it reads as released.
   *
   * @param mappingSlotIndex Slot of the mapping.
   */
  void release(uint32_t mappingSlotIndex);

  /**
   * Invalidates every mapping of \p bufferSlotIndex, called when the buffer is retired so a
   * mapping cannot outlive the allocation it points into.
   *
   * @param bufferSlotIndex Slot of the buffer being retired.
   */
  void invalidateBuffer(uint32_t bufferSlotIndex);

  /// How many mappings are currently open, counting ones whose buffer was retired: those are
  /// still open handles that have to be released. Test accessor for leak checks.
  [[nodiscard]] size_t liveMappingCountForTest() const;

private:
  /// One open mapping.
  struct Entry {
    bool live = false;             //!< Whether this slot holds a mapping at all.
    bool invalidated = false;      //!< Whether the mapped buffer was retired underneath it.
    uint32_t bufferSlotIndex = 0;  //!< Slot of the mapped buffer.
    uint64_t offsetBytes = 0;      //!< Byte offset of the mapped range.
    uint64_t byteCount = 0;        //!< Length of the mapped range.
    uint64_t readySerial = 0;      //!< Submission that must complete before the range is readable.
  };

  /// Returns the live entry at \p mappingSlotIndex, or nullptr. @param mappingSlotIndex Slot.
  const Entry* find(uint32_t mappingSlotIndex) const;

  /// What \p entry would report right now, shared by waiting and reading so the two cannot
  /// disagree about whether a mapping is readable. @param entry Entry to judge; may be nullptr.
  MapSliceState readiness(const Entry* entry) const;

  BufferMappingHost& host_;     //!< Supplies the backend's buffer facts.
  std::vector<Entry> entries_;  //!< Indexed by mapping slot index.
};

}  // namespace donner::gpu
