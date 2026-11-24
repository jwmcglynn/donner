#pragma once
/// @file
/// Task records, and the bounded pools they are allocated from.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/base/tasks/FalseSharing.h"
#include "donner/base/tasks/Parker.h"
#include "donner/base/tasks/SchedulerWork.h"
#include "donner/base/tasks/ScopeStop.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DONNER_TASKS_RECORD_ASAN 1
#endif
#endif
#if !defined(DONNER_TASKS_RECORD_ASAN) && defined(__SANITIZE_ADDRESS__)
#define DONNER_TASKS_RECORD_ASAN 1
#endif

#if defined(DONNER_TASKS_RECORD_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace donner::tasks {

class AsyncTaskScope;

/**
 * One spawned task: its closure, and what the thread that finishes it needs.
 *
 * Layout: an intrusive link and work kind, its allocating pool, completion and stop metadata,
 * the closure's call function, and 64 bytes of inline closure storage. A closure that does not fit,
 * or needs more than `max_align_t` alignment, is moved into a heap block that the record owns. The
 * record is 128 bytes, two cache lines.
 *
 * Records are recycled by their pool. While a record is free, everything after its intrusive link
 * is poisoned in AddressSanitizer builds, so a thread that touches a record after returning it
 * fails there.
 */
class alignas(64) TaskRecord : public SchedulerWork {
public:
  /// Bytes of closure storage inside the record.
  static constexpr std::size_t kInlineBytes = 64;

  /// Alignment of the inline closure storage.
  static constexpr std::size_t kInlineAlignment = alignof(std::max_align_t);

  /// Whether a closure of type \p F is stored inline rather than in a heap block.
  template <typename F>
  static constexpr bool kStoredInline =
      sizeof(std::decay_t<F>) <= kInlineBytes && alignof(std::decay_t<F>) <= kInlineAlignment;

  TaskRecord() : SchedulerWork(Kind::Record) {}

  TaskRecord(const TaskRecord&) = delete;
  TaskRecord(TaskRecord&&) = delete;
  TaskRecord& operator=(const TaskRecord&) = delete;
  TaskRecord& operator=(TaskRecord&&) = delete;

  ~TaskRecord() {
    UTILS_RELEASE_ASSERT_MSG(call_ == nullptr, "A task record was destroyed holding a closure");
  }

  /**
   * Stores \p work as the record's closure. The record must hold none.
   *
   * @param work Callable with no arguments.
   */
  template <typename F>
  void setClosure(F&& work) {
    using Fn = std::decay_t<F>;
    static_assert(std::is_invocable_v<Fn&>, "A task closure is called with no arguments");
    UTILS_RELEASE_ASSERT_MSG(call_ == nullptr, "A task record already holds a closure");
    if constexpr (kStoredInline<Fn>) {
      ::new (static_cast<void*>(storage_)) Fn(std::forward<F>(work));
    } else {
      ::new (static_cast<void*>(storage_)) Fn*(new Fn(std::forward<F>(work)));
    }
    call_ = &Call<Fn>;
  }

  /// Runs the closure, then destroys it. The record must hold one.
  void run() { finish(Action::Run); }

  /// Destroys the closure without running it. The record must hold one.
  void discard() { finish(Action::Discard); }

  /// Whether the record holds a closure.
  [[nodiscard]] bool hasClosure() const { return call_ != nullptr; }

  /**
   * Records what the finisher of this task touches: its scope's counter and the parker the scope
   * recorded; and the scope's stop request, which the thread that takes the task checks first.
   *
   * @param scopeCounter The scope's counter word.
   * @param parker The parker to change when the counter reaches zero.
   * @param stop The scope's stop request, or null for a scope without one.
   */
  void setScope(std::atomic<std::uint64_t>* scopeCounter, Parker<>* parker, const ScopeStop* stop) {
    scopeCounter_ = scopeCounter;
    parker_ = parker;
    stop_ = stop;
  }

  /// Records the asynchronous scope to notify after returning this record.
  void setAsyncScope(AsyncTaskScope* scope) { asyncScope_ = scope; }

  [[nodiscard]] AsyncTaskScope* asyncScope() const { return asyncScope_; }

  /// The scope counter set by \ref setScope, or null.
  [[nodiscard]] std::atomic<std::uint64_t>* scopeCounter() const { return scopeCounter_; }

  /// The parker set by \ref setScope, or null.
  [[nodiscard]] Parker<>* parker() const { return parker_; }

  /// The scope's stop request set by \ref setScope, or null.
  [[nodiscard]] const ScopeStop* stop() const { return stop_; }

  /// The intrusive link, owned by whichever list holds the record.
  [[nodiscard]] TaskRecord* next() const { return static_cast<TaskRecord*>(SchedulerWork::next); }

  /// Sets the intrusive link.
  void setNext(TaskRecord* next) { SchedulerWork::next = next; }

  /// The identity of the pool that allocated the record.
  [[nodiscard]] const void* pool() const { return pool_; }

private:
  template <typename>
  friend class CreatorRecordPool;
  friend class RecordStore;

  enum class Action : std::uint8_t { Run, Discard };

  template <typename Fn>
  static void Call(TaskRecord& record, Action action) {
    if constexpr (kStoredInline<Fn>) {
      Fn& fn = *std::launder(reinterpret_cast<Fn*>(record.storage_));
      if (action == Action::Run) {
        fn();
      }
      fn.~Fn();
    } else {
      Fn* fn = *std::launder(reinterpret_cast<Fn**>(record.storage_));
      if (action == Action::Run) {
        (*fn)();
      }
      delete fn;
    }
  }

  void finish(Action action) {
    UTILS_RELEASE_ASSERT_MSG(call_ != nullptr, "A task record has no closure to finish");
    void (*call)(TaskRecord&, Action) = call_;
    call_ = nullptr;
    call(*this, action);
  }

  /// Clears the owner's state and records the allocating pool.
  void resetForAllocation(const void* pool) {
    SchedulerWork::next = nullptr;
    pool_ = pool;
    scopeCounter_ = nullptr;
    parker_ = nullptr;
    asyncScope_ = nullptr;
    stop_ = nullptr;
  }

  /// Marks the record free: everything after the intrusive link becomes unaddressable under
  /// AddressSanitizer.
  void poisonWhileFree() {
#if defined(DONNER_TASKS_RECORD_ASAN)
    ASAN_POISON_MEMORY_REGION(&this->SchedulerWork::next + 1, PayloadBytes());
#endif
  }

  /// Makes a free record addressable again before it is reused or deleted.
  void unpoison() {
#if defined(DONNER_TASKS_RECORD_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(&this->SchedulerWork::next + 1, PayloadBytes());
#endif
  }

  /// Bytes after the intrusive link, which alone stays usable while the record is on a free list.
  static constexpr std::size_t PayloadBytes() { return sizeof(TaskRecord) - sizeof(TaskRecord*); }

  const void* pool_ = nullptr;
  std::atomic<std::uint64_t>* scopeCounter_ = nullptr;
  Parker<>* parker_ = nullptr;
  AsyncTaskScope* asyncScope_ = nullptr;
  const ScopeStop* stop_ = nullptr;
  void (*call_)(TaskRecord&, Action) = nullptr;
  alignas(kInlineAlignment) std::byte storage_[kInlineBytes];
};

static_assert(sizeof(TaskRecord) == 128, "A task record is two cache lines");
static_assert(!std::is_copy_constructible_v<TaskRecord> &&
                  !std::is_move_constructible_v<TaskRecord>,
              "Lists and queues hold pointers to task records, so a record never moves");

/**
 * The records one pool has created, its free list and its live count, used by one thread at a
 * time: the owning creator for \ref CreatorRecordPool, the holder of the guard mutex for
 * \ref ExternalRecordPool.
 */
class RecordStore {
public:
  /**
   * @param maxLiveRecords The most records the pool creates; a record is live from allocation
   *   until it is back on the free list.
   */
  explicit RecordStore(std::size_t maxLiveRecords) : maxLive_(maxLiveRecords) {
    UTILS_RELEASE_ASSERT_MSG(maxLiveRecords > 0, "A task record pool needs a bound above zero");
  }

  RecordStore(const RecordStore&) = delete;
  RecordStore& operator=(const RecordStore&) = delete;

  /// Deletes every record. Every record must be back on the free list.
  ~RecordStore() {
    UTILS_RELEASE_ASSERT_MSG(live_ == 0, "A task record pool was destroyed with live records");
    while (TaskRecord* record = freeHead_) {
      freeHead_ = record->next();
      record->unpoison();
      delete record;
    }
  }

  /**
   * Takes a record from the free list, or creates one while fewer than the bound exist.
   *
   * @param pool Identity stored in the record.
   * @return The record, or null when the bound is reached and the free list is empty.
   */
  [[nodiscard]] TaskRecord* allocate(const void* pool) {
    TaskRecord* record = freeHead_;
    if (record != nullptr) {
      freeHead_ = record->next();
      record->unpoison();
    } else if (created_ < maxLive_) {
      record = new TaskRecord;
      ++created_;
    } else {
      return nullptr;
    }
    ++live_;
    record->resetForAllocation(pool);
    return record;
  }

  /// Puts one record back on the free list. It must hold no closure.
  void release(TaskRecord* record) {
    UTILS_RELEASE_ASSERT_MSG(live_ > 0 && record != freeHead_, "A task record was returned twice");
    UTILS_RELEASE_ASSERT_MSG(!record->hasClosure(), "A task record was returned holding a closure");
    pushFree(record);
    --live_;
  }

  /**
   * Puts a list of records, linked through their links and already poisoned, back on the free
   * list.
   *
   * @param head First record of the list.
   * @return The number of records.
   */
  std::size_t releaseList(TaskRecord* head) {
    std::size_t count = 0;
    while (head != nullptr) {
      TaskRecord* next = head->next();
      head->setNext(freeHead_);
      freeHead_ = head;
      head = next;
      ++count;
      // A record returned twice can make the list longer than the live count.
      UTILS_RELEASE_ASSERT_MSG(count <= live_, "More task records came back than were live");
    }
    live_ -= count;
    return count;
  }

  /// Whether the free list holds a record.
  [[nodiscard]] bool hasFreeRecord() const { return freeHead_ != nullptr; }

  /// Records allocated and not yet back on the free list.
  [[nodiscard]] std::size_t liveCount() const { return live_; }

  /// Records created so far; never above \ref maxLiveRecords.
  [[nodiscard]] std::size_t createdCount() const { return created_; }

  /// The bound on live records.
  [[nodiscard]] std::size_t maxLiveRecords() const { return maxLive_; }

private:
  void pushFree(TaskRecord* record) {
    record->setNext(freeHead_);
    record->poisonWhileFree();
    freeHead_ = record;
  }

  const std::size_t maxLive_;
  TaskRecord* freeHead_ = nullptr;
  std::size_t live_ = 0;
  std::size_t created_ = 0;
};

/**
 * Test seam with a hook at each step of the return stack where a return and the owner's take can
 * interleave. Production code uses this policy, whose hooks compile to nothing.
 */
struct NoRecordPoolHooks {
  /// Called by \ref CreatorRecordPool::returnFromOtherThread after it linked the record to the head
  /// it read, before the compare-and-swap that publishes it.
  static void BeforeReturnPublishes() {}

  /// Called by the owner when it found returned records, before the exchange that takes them.
  static void BeforeOwnerTakesReturns() {}
};

/**
 * The task records of one creator, a worker or an adoptable slot: at most `maxTasksPerCreator`
 * live records, allocated by the creator alone.
 *
 * The creator allocates, and returns the records it ran itself, with no atomic operation. A record
 * that another thread ran is returned by that thread onto the pool's return stack, a lock-free
 * multi-producer stack with one consumer: each return links the record to the head it read and
 * publishes it with a release compare-and-swap, and the creator takes the whole stack with one
 * acquire exchange when its free list runs empty. Since nothing pops single entries, a head that
 * is taken, reused and returned again cannot corrupt a return that read it earlier: the
 * compare-and-swap succeeds only if that record is the head again, and the record then links to
 * it, which is correct.
 *
 * A record is counted live from allocation until the creator takes it back, so a returned record
 * still counts until the creator's next take. When the free list and the return stack are empty
 * and the bound is reached, \ref tryAllocate returns null and the caller runs the task inline.
 *
 * @tparam Hooks Test seam, see \ref NoRecordPoolHooks.
 */
template <typename Hooks = NoRecordPoolHooks>
class CreatorRecordPool {
public:
  /// @param maxLiveRecords The bound on live records, above zero.
  explicit CreatorRecordPool(std::size_t maxLiveRecords) : store_(maxLiveRecords) {}

  CreatorRecordPool(const CreatorRecordPool&) = delete;
  CreatorRecordPool(CreatorRecordPool&&) = delete;
  CreatorRecordPool& operator=(const CreatorRecordPool&) = delete;
  CreatorRecordPool& operator=(CreatorRecordPool&&) = delete;

  /// Creator only, after every other thread has returned its records. Fails a release assertion if
  /// any record is still live.
  ~CreatorRecordPool() { reclaimReturned(); }

  /**
   * Allocates a record. Creator only. Takes back returned records first when the free list is
   * empty.
   *
   * @return The record, cleared and holding no closure; or null when the bound is reached, in which
   *   case the caller runs the task inline.
   */
  [[nodiscard]] TaskRecord* tryAllocate() {
    if (!store_.hasFreeRecord()) {
      reclaimReturned();
    }
    return store_.allocate(this);
  }

  /// Returns a record the creator ran itself. Creator only.
  void releaseFromCreator(TaskRecord* record) { store_.release(record); }

  /**
   * Returns a record that this thread ran. Any thread but the creator. The calling thread must not
   * touch the record afterwards.
   *
   * @param record A record of this pool holding no closure.
   */
  void returnFromOtherThread(TaskRecord* record) {
    UTILS_RELEASE_ASSERT_MSG(!record->hasClosure(), "A task record was returned holding a closure");
    UTILS_RELEASE_ASSERT_MSG(record->pool() == this, "A task record was returned to another pool");
    record->poisonWhileFree();
    TaskRecord* head = returned_.load(std::memory_order_relaxed);
    while (true) {
      UTILS_RELEASE_ASSERT_MSG(head != record, "A task record was returned twice");
      record->setNext(head);
      Hooks::BeforeReturnPublishes();
      // Release: the returner's last writes to the record happen before the creator reuses it.
      if (returned_.compare_exchange_weak(head, record, std::memory_order_release,
                                          std::memory_order_relaxed)) {
        return;
      }
    }
  }

  /**
   * Moves every returned record to the free list. Creator only.
   *
   * @return The number of records taken back.
   */
  std::size_t reclaimReturned() {
    if (returned_.load(std::memory_order_relaxed) == nullptr) {
      return 0;
    }
    Hooks::BeforeOwnerTakesReturns();
    // Acquire: pairs with each return's release compare-and-swap.
    TaskRecord* head = returned_.exchange(nullptr, std::memory_order_acquire);
    return store_.releaseList(head);
  }

  /// Records allocated and not yet taken back. Creator only.
  [[nodiscard]] std::size_t liveCount() const { return store_.liveCount(); }

  /// Records created so far; never above \ref maxLiveRecords. Creator only.
  [[nodiscard]] std::size_t createdCount() const { return store_.createdCount(); }

  /// The bound on live records.
  [[nodiscard]] std::size_t maxLiveRecords() const { return store_.maxLiveRecords(); }

private:
  RecordStore store_;
  /// The return stack's head; on its own cache line, since every returner writes it.
  alignas(kFalseSharingBytes) std::atomic<TaskRecord*> returned_{nullptr};
};

/**
 * The task records of every thread that is neither a worker nor an adopted thread: at most
 * `maxExternalTasks` live records, shared by all of them and used only while the caller holds the
 * guard mutex, the global queue's. Each call takes the caller's lock as proof and fails a release
 * assertion if it does not hold the guard.
 */
class ExternalRecordPool {
public:
  /**
   * @param guard The mutex every call must hold.
   * @param maxLiveRecords The bound on live records, above zero.
   */
  ExternalRecordPool(std::mutex& guard, std::size_t maxLiveRecords)
      : guard_(guard), store_(maxLiveRecords) {}

  ExternalRecordPool(const ExternalRecordPool&) = delete;
  ExternalRecordPool& operator=(const ExternalRecordPool&) = delete;

  /// Fails a release assertion if any record is still live.
  ~ExternalRecordPool() = default;

  /**
   * Allocates a record.
   *
   * @param held A lock holding the guard mutex.
   * @return The record, or null when the bound is reached, in which case the caller runs the task
   *   inline.
   */
  [[nodiscard]] TaskRecord* tryAllocate(const std::unique_lock<std::mutex>& held) {
    checkHeld(held);
    return store_.allocate(this);
  }

  /**
   * Returns a record.
   *
   * @param record A record of this pool holding no closure.
   * @param held A lock holding the guard mutex.
   */
  void release(TaskRecord* record, const std::unique_lock<std::mutex>& held) {
    checkHeld(held);
    UTILS_RELEASE_ASSERT_MSG(record->pool() == this, "A task record was returned to another pool");
    store_.release(record);
  }

  /// Records allocated and not yet returned. @param held A lock holding the guard mutex.
  [[nodiscard]] std::size_t liveCount(const std::unique_lock<std::mutex>& held) const {
    checkHeld(held);
    return store_.liveCount();
  }

  /// The bound on live records.
  [[nodiscard]] std::size_t maxLiveRecords() const { return store_.maxLiveRecords(); }

private:
  void checkHeld(const std::unique_lock<std::mutex>& held) const {
    UTILS_RELEASE_ASSERT_MSG(held.mutex() == &guard_ && held.owns_lock(),
                             "The external task record pool is used without its guard mutex");
  }

  std::mutex& guard_;
  RecordStore store_;
};

}  // namespace donner::tasks
