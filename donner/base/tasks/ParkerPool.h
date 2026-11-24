#pragma once
/// @file
/// Parkers for threads that are not workers, owned by a pool that outlives every thread holding
/// one of its slots.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "donner/base/tasks/FalseSharing.h"
#include "donner/base/tasks/Parker.h"

namespace donner::tasks {

/**
 * The parkers of one scheduler's non-worker scope creators: slots at stable addresses, allocated
 * in fixed blocks, and a free list under a mutex.
 *
 * The scheduler creates its pool and shares it by `std::shared_ptr`. A thread that creates a scope
 * gets its slot through \ref ParkerForThread, which keeps a thread-local map from the scheduler's
 * generation number (unique for the life of the process, unlike an address) to an entry holding a
 * reference to the pool and the slot. When the thread exits, the map's destructor returns each slot
 * and drops each reference, so a pool outlives every thread that holds one of its slots, and a
 * scheduler destroyed first leaves nothing dangling. The scheduler marks its pool closed once every
 * worker is joined; a thread registering with a scheduler first drops every entry whose pool is
 * closed, so it holds pools only for schedulers that were open at its last registration, plus the
 * one it registers with.
 *
 * A finisher that read a slot's parker before the slot passed to another thread can still change
 * it; the new owner then sees one spurious wake, which it re-checks.
 */
class ParkerPool {
public:
  /// A slot handed out by \ref acquire.
  struct Slot {
    std::size_t index;  //!< The slot's index, to pass back to \ref release.
    Parker<>* parker;   //!< The slot's parker, at a stable address while the pool lives.
  };

  ParkerPool();

  ParkerPool(const ParkerPool&) = delete;
  ParkerPool(ParkerPool&&) = delete;
  ParkerPool& operator=(const ParkerPool&) = delete;
  ParkerPool& operator=(ParkerPool&&) = delete;

  ~ParkerPool();

  /// Hands out a free slot, adding a block when none is free.
  [[nodiscard]] Slot acquire();

  /// Returns the slot at \p index to the free list.
  void release(std::size_t index);

  /// Marks the pool closed: no scope of its scheduler is live, so none of its slots is in use.
  void close();

  /// Whether \ref close was called.
  [[nodiscard]] bool closed() const;

  /// Slots created so far, in use or free.
  [[nodiscard]] std::size_t slotCount() const;

  /// Slots handed out and not yet returned.
  [[nodiscard]] std::size_t slotsInUse() const;

  /**
   * The calling thread's parker for the scheduler with generation \p generation, registering the
   * thread with \p pool on the first call for that generation. Registration first drops the
   * thread's entries for closed pools.
   *
   * The map is a thread-local object, so calling this while the calling thread's own thread-local
   * objects are being destroyed (from another thread-local's destructor, at thread exit) is
   * undefined: the map may already be gone. Creating a \ref TaskScope there is the same call.
   *
   * @param generation The scheduler's generation number, from \ref NextGeneration.
   * @param pool The scheduler's pool.
   */
  static Parker<>* ParkerForThread(std::uint64_t generation,
                                   const std::shared_ptr<ParkerPool>& pool);

  /// A generation number no scheduler in this process has had.
  static std::uint64_t NextGeneration();

  /// Pools alive in the process, for tests.
  static std::size_t LiveCountForTesting();

private:
  static constexpr std::size_t kBlockSlots = 32;

  /// One parker on its own cache line, so threads sleeping on neighbouring slots share none.
  struct alignas(kFalseSharingBytes) PaddedParker {
    Parker<> parker;
  };

  struct Block {
    PaddedParker slots[kBlockSlots];
  };

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Block>> blocks_;
  std::vector<std::size_t> free_;
  /// Whether each slot is handed out, indexed by slot, so a return is checked in constant time.
  std::vector<bool> slotInUse_;
  std::size_t inUse_ = 0;
  std::atomic<bool> closed_{false};
};

}  // namespace donner::tasks
