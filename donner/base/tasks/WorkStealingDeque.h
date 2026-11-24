#pragma once
/// @file
/// A fixed-capacity Chase-Lev work-stealing deque.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <type_traits>

#include "donner/base/Utils.h"
#include "donner/base/tasks/FalseSharing.h"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DONNER_TASKS_DEQUE_TSAN 1
#endif
#endif
#if !defined(DONNER_TASKS_DEQUE_TSAN) && defined(__SANITIZE_THREAD__)
#define DONNER_TASKS_DEQUE_TSAN 1
#endif

namespace donner::tasks {

/// The memory orderings a \ref WorkStealingDeque uses on its indices.
enum class DequeOrdering : std::uint8_t {
  /**
   * The C11 orderings of Le, Pop, Cohen and Zappa Nardelli, "Correct and Efficient Work-Stealing
   * for Weak Memory Models" (PPoPP 2013), Figure 1: relaxed and release-acquire index accesses,
   * a release fence in `push`, and sequentially consistent fences in `take` and `steal`. They are
   * proven correct for the Arm and POWER memory models. This is the production form.
   */
  WeakMemory,
  /**
   * Every index access sequentially consistent and no standalone fences. ThreadSanitizer does not
   * model standalone fences, so this is the form it can check: it sees every ordering the deque
   * relies on as an ordinary atomic. The default in ThreadSanitizer builds.
   */
  SequentiallyConsistent,
};

/// Ostream output operator for \ref DequeOrdering, for test diagnostics.
inline std::ostream& operator<<(std::ostream& os, DequeOrdering ordering) {
  switch (ordering) {
    case DequeOrdering::WeakMemory: return os << "WeakMemory";
    case DequeOrdering::SequentiallyConsistent: return os << "SequentiallyConsistent";
  }
  UTILS_UNREACHABLE();
}

/// The ordering form a \ref WorkStealingDeque uses unless one is named.
#if defined(DONNER_TASKS_DEQUE_TSAN)
inline constexpr DequeOrdering kDefaultDequeOrdering = DequeOrdering::SequentiallyConsistent;
#else
inline constexpr DequeOrdering kDefaultDequeOrdering = DequeOrdering::WeakMemory;
#endif

/// How a \ref WorkStealingDeque::steal call ended.
enum class StealStatus : std::uint8_t {
  Taken,  //!< The thief removed the oldest entry.
  Empty,  //!< The deque held no entry when the thief looked.
  Lost,   //!< Another thief, or the owner, claimed the entry first; the deque may still hold more.
};

/// Ostream output operator for \ref StealStatus, for test diagnostics.
inline std::ostream& operator<<(std::ostream& os, StealStatus status) {
  switch (status) {
    case StealStatus::Taken: return os << "Taken";
    case StealStatus::Empty: return os << "Empty";
    case StealStatus::Lost: return os << "Lost";
  }
  UTILS_UNREACHABLE();
}

/**
 * The result of \ref WorkStealingDeque::steal.
 *
 * @tparam T The deque's entry type.
 */
template <typename T>
struct StealResult {
  StealStatus status = StealStatus::Empty;  //!< How the steal ended.
  T item{};  //!< The stolen entry when \ref status is \ref StealStatus::Taken, else `T{}`.

  /// Equality operator, comparing both fields.
  friend bool operator==(const StealResult& lhs, const StealResult& rhs) = default;

  /// Ostream output operator, for test diagnostics.
  friend std::ostream& operator<<(std::ostream& os, const StealResult& result) {
    os << result.status;
    if (result.status == StealStatus::Taken) {
      os << "(" << result.item << ")";
    }
    return os;
  }
};

/**
 * Test seam with a hook at each claim of a contended entry. Production code uses this policy,
 * whose hooks compile to nothing; deterministic interleaving tests pass a policy that pauses the
 * calling thread inside the hook.
 */
struct NoDequeHooks {
  /// Called by `take` when it found exactly one entry, before its compare-and-swap on the top
  /// index.
  static void BeforeTakeClaimsLastEntry() {}

  /// Called by `steal` after it read the oldest entry, before its compare-and-swap on the top
  /// index.
  static void BeforeStealClaims() {}
};

/**
 * A Chase-Lev work-stealing deque on a fixed ring of slots.
 *
 * One thread, the owner, pushes and takes at the bottom; any number of other threads, thieves,
 * steal from the top. The owner takes the newest entry and thieves take the oldest. The owner may
 * change between threads only across a happens-before edge, such as a thread start or join.
 *
 * The top and bottom indices are signed 64-bit logical positions that are never wrapped or reset;
 * they would overflow only after 2^63 pushes. The top only grows, and the bottom grows on a push
 * and shrinks on a take. An entry's slot is its logical index modulo the capacity. The bottom is
 * below the top only for a moment inside `take`, which signed indices allow without underflow even
 * on a new empty deque. Only the last entry is contended between the owner and a thief, and it is
 * claimed by a compare-and-swap on the top index.
 *
 * The ring is allocated once and never resized, so no thief can read a retired array. A push that
 * would exceed the capacity fails and leaves the deque unchanged; the caller keeps the entry.
 *
 * @tparam T Entry type, trivially copyable with lock-free atomics, normally a pointer. Slots are
 *   relaxed atomics, so a thief reading a slot the owner is rewriting is not a data race.
 * @tparam kOrdering The index ordering form, see \ref DequeOrdering.
 * @tparam Hooks Test seam, see \ref NoDequeHooks.
 */
template <typename T, DequeOrdering kOrdering = kDefaultDequeOrdering,
          typename Hooks = NoDequeHooks>
class WorkStealingDeque {
  static_assert(std::is_trivially_copyable_v<T>, "Deque entries are copied through atomics");
  static_assert(std::is_default_constructible_v<T>, "StealResult needs an empty entry value");
  static_assert(std::atomic<T>::is_always_lock_free, "Deque slots must be lock-free atomics");

public:
  /// Logical position of an entry. It never wraps: a signed 64-bit index overflows only after 2^63
  /// pushes.
  using Index = std::int64_t;

  /// The largest supported capacity.
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;

  /**
   * Allocates a ring of \p capacity slots.
   *
   * @param capacity Number of entries the deque holds at most; a power of two, at most
   *   \ref kMaxCapacity. Any other value fails a release assertion.
   */
  explicit WorkStealingDeque(std::size_t capacity)
      : mask_(CheckedCapacity(capacity) - 1),
        slots_(std::make_unique<std::atomic<T>[]>(capacity)) {}

  WorkStealingDeque(const WorkStealingDeque&) = delete;
  WorkStealingDeque(WorkStealingDeque&&) = delete;
  WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;
  WorkStealingDeque& operator=(WorkStealingDeque&&) = delete;
  ~WorkStealingDeque() = default;

  /// The number of entries the deque holds at most.
  [[nodiscard]] std::size_t capacity() const { return mask_ + 1; }

  /**
   * Pushes \p item at the bottom. Owner only.
   *
   * The fullness check reads the top index, which a concurrent steal may be advancing, so a push
   * racing a steal can fail even though the steal is freeing a slot. It never succeeds past the
   * capacity.
   *
   * @param item Entry to push.
   * @return True if the entry was pushed, false if the deque was full.
   */
  [[nodiscard]] bool push(T item) {
    const Index bottom = bottom_.load(IndexOrder(std::memory_order_relaxed));
    // Acquire: a thief's read of the slot it claimed happens before the owner rewrites that slot.
    const Index top = top_.load(IndexOrder(std::memory_order_acquire));
    if (bottom - top >= static_cast<Index>(capacity())) {
      return false;
    }

    slots_[slotOf(bottom)].store(item, std::memory_order_relaxed);
    // Publishes the slot, and whatever the entry points to, to a thief that reads the new bottom.
    Fence(std::memory_order_release);
    bottom_.store(bottom + 1, IndexOrder(std::memory_order_relaxed));
    return true;
  }

  /**
   * Whether a push would fail now. Owner only. Only the owner pushes and a thief only frees slots,
   * so a push the owner makes after this returned false succeeds.
   */
  [[nodiscard]] bool full() const {
    const Index bottom = bottom_.load(IndexOrder(std::memory_order_relaxed));
    // Acquire, as in push: a thief's read of the slot it claimed happens before the owner reuses
    // it.
    const Index top = top_.load(IndexOrder(std::memory_order_acquire));
    return bottom - top >= static_cast<Index>(capacity());
  }

  /**
   * Takes the newest entry from the bottom. Owner only.
   *
   * @return The entry, or `std::nullopt` if the deque was empty or a thief claimed its last entry.
   */
  [[nodiscard]] std::optional<T> take() {
    const Index bottom = bottom_.load(IndexOrder(std::memory_order_relaxed)) - 1;
    bottom_.store(bottom, IndexOrder(std::memory_order_relaxed));
    // Store-load barrier: either a thief sees the lowered bottom or the owner sees the thief's
    // claim.
    Fence(std::memory_order_seq_cst);
    Index top = top_.load(IndexOrder(std::memory_order_relaxed));

    if (top > bottom) {
      bottom_.store(bottom + 1, IndexOrder(std::memory_order_relaxed));
      return std::nullopt;
    }

    const T item = slots_[slotOf(bottom)].load(std::memory_order_relaxed);
    if (top < bottom) {
      return item;
    }

    Hooks::BeforeTakeClaimsLastEntry();
    // A failed claim means a thief took the entry; it carries no data to the owner.
    const bool claimed = top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                                      IndexOrder(std::memory_order_relaxed));
    bottom_.store(bottom + 1, IndexOrder(std::memory_order_relaxed));
    if (!claimed) {
      return std::nullopt;
    }
    return item;
  }

  /**
   * Steals the oldest entry from the top. Any thread other than the owner.
   *
   * @return The entry with \ref StealStatus::Taken; \ref StealStatus::Empty if the deque held
   *   none; or \ref StealStatus::Lost if another thread claimed it first, in which case the deque
   *   may still hold entries and the caller may retry.
   */
  [[nodiscard]] StealResult<T> steal() {
    Index top = top_.load(IndexOrder(std::memory_order_acquire));
    // Store-load barrier paired with the one in take.
    Fence(std::memory_order_seq_cst);
    // Acquire: pairs with push's release fence, so the slot and the entry's contents are visible.
    const Index bottom = bottom_.load(IndexOrder(std::memory_order_acquire));
    if (top >= bottom) {
      return StealResult<T>{StealStatus::Empty, T{}};
    }

    const T item = slots_[slotOf(top)].load(std::memory_order_relaxed);
    Hooks::BeforeStealClaims();
    // A failed claim takes nothing, so its read needs no ordering.
    if (!top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                      IndexOrder(std::memory_order_relaxed))) {
      return StealResult<T>{StealStatus::Lost, T{}};
    }
    return StealResult<T>{StealStatus::Taken, item};
  }

private:
  static constexpr bool kSequentiallyConsistent =
      kOrdering == DequeOrdering::SequentiallyConsistent;

  /// The ordering of an index access: \p weakOrder in the production form, else seq_cst.
  static constexpr std::memory_order IndexOrder(std::memory_order weakOrder) {
    return kSequentiallyConsistent ? std::memory_order_seq_cst : weakOrder;
  }

  /// A standalone fence in the production form; nothing in the sequentially consistent form.
  static void Fence(std::memory_order order) {
    if constexpr (!kSequentiallyConsistent) {
      std::atomic_thread_fence(order);
    }
  }

  static std::size_t CheckedCapacity(std::size_t capacity) {
    UTILS_RELEASE_ASSERT_MSG(capacity > 0 && (capacity & (capacity - 1)) == 0,
                             "WorkStealingDeque capacity must be a power of two");
    UTILS_RELEASE_ASSERT_MSG(capacity <= kMaxCapacity,
                             "WorkStealingDeque capacity exceeds kMaxCapacity");
    return capacity;
  }

  std::size_t slotOf(Index index) const { return static_cast<std::size_t>(index) & mask_; }

  const std::size_t mask_;
  const std::unique_ptr<std::atomic<T>[]> slots_;
  alignas(kFalseSharingBytes) std::atomic<Index> top_{0};
  alignas(kFalseSharingBytes) std::atomic<Index> bottom_{0};
};

static_assert(!std::is_copy_constructible_v<WorkStealingDeque<void*>> &&
                  !std::is_move_constructible_v<WorkStealingDeque<void*>>,
              "Thieves hold references to a deque, so it never moves");

}  // namespace donner::tasks
