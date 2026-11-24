#include "donner/base/tasks/TaskRecord.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {
namespace {

using testing::AllOf;
using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::IsNull;
using testing::Le;
using testing::NotNull;
using testing::UnorderedElementsAreArray;

/// Counts constructions, destructions and calls of closures that carry it.
struct ClosureCounts {
  int calls = 0;
  int destructions = 0;

  friend bool operator==(const ClosureCounts&, const ClosureCounts&) = default;

  friend std::ostream& operator<<(std::ostream& os, const ClosureCounts& counts) {
    return os << "{calls=" << counts.calls << " destructions=" << counts.destructions << "}";
  }
};

/// A closure of \p kBytes bytes that records its call and its destruction.
template <std::size_t kBytes>
struct CountingClosure {
  explicit CountingClosure(ClosureCounts* counts) : counts(counts) {}
  CountingClosure(CountingClosure&& other) noexcept : counts(other.counts) {
    other.counts = nullptr;
  }
  CountingClosure(const CountingClosure&) = delete;
  CountingClosure& operator=(const CountingClosure&) = delete;
  CountingClosure& operator=(CountingClosure&&) = delete;
  ~CountingClosure() {
    if (counts != nullptr) {
      ++counts->destructions;
    }
  }

  void operator()() { ++counts->calls; }

  ClosureCounts* counts;
  std::array<std::byte, kBytes - sizeof(ClosureCounts*)> padding{};
};

struct alignas(64) OverAlignedClosure {
  void operator()() const {}
};

static_assert(TaskRecord::kStoredInline<CountingClosure<64>>, "A 64-byte closure is inline");
static_assert(!TaskRecord::kStoredInline<CountingClosure<72>>, "A 72-byte closure is on the heap");
static_assert(!TaskRecord::kStoredInline<OverAlignedClosure>,
              "A closure aligned beyond max_align_t is on the heap");

// Single-thread model tests of a record's closure.

TEST(TaskRecordTest, AnInlineClosureRunsOnceAndIsDestroyedOnce) {
  TaskRecord record;
  ClosureCounts counts;
  record.setClosure(CountingClosure<64>(&counts));
  EXPECT_TRUE(record.hasClosure());
  EXPECT_THAT(counts, Eq(ClosureCounts{0, 0}));

  record.run();
  EXPECT_FALSE(record.hasClosure());
  EXPECT_THAT(counts, Eq(ClosureCounts{1, 1}));
}

TEST(TaskRecordTest, AHeapClosureRunsOnceAndIsDestroyedOnce) {
  TaskRecord record;
  ClosureCounts counts;
  record.setClosure(CountingClosure<200>(&counts));
  record.run();
  EXPECT_FALSE(record.hasClosure());
  EXPECT_THAT(counts, Eq(ClosureCounts{1, 1}));
}

TEST(TaskRecordTest, DiscardDestroysTheClosureWithoutRunningIt) {
  for (const bool heap : {false, true}) {
    TaskRecord record;
    ClosureCounts counts;
    if (heap) {
      record.setClosure(CountingClosure<200>(&counts));
    } else {
      record.setClosure(CountingClosure<16>(&counts));
    }
    record.discard();
    EXPECT_FALSE(record.hasClosure());
    EXPECT_THAT(counts, Eq(ClosureCounts{0, 1})) << "heap=" << heap;
  }
}

TEST(TaskRecordTest, AnOverAlignedClosureRuns) {
  TaskRecord record;
  int calls = 0;
  struct alignas(64) Closure {
    int* calls;
    void operator()() const { ++*calls; }
  };
  record.setClosure(Closure{&calls});
  record.run();
  EXPECT_THAT(calls, Eq(1));
}

TEST(TaskRecordTest, ARecordCanHoldANewClosureAfterTheLastOneFinished) {
  TaskRecord record;
  ClosureCounts first;
  ClosureCounts second;
  record.setClosure(CountingClosure<200>(&first));
  record.run();
  record.setClosure(CountingClosure<16>(&second));
  record.run();
  EXPECT_THAT(first, Eq(ClosureCounts{1, 1}));
  EXPECT_THAT(second, Eq(ClosureCounts{1, 1}));
}

TEST(TaskRecordDeathTest, ClosureMisuseFailsAReleaseAssertion) {
  EXPECT_DEATH(
      {
        TaskRecord record;
        record.setClosure([] {});
        record.setClosure([] {});
      },
      "already holds a closure");
  EXPECT_DEATH(
      {
        TaskRecord record;
        record.run();
      },
      "no closure to finish");
  EXPECT_DEATH(
      {
        TaskRecord record;
        record.setClosure([] {});
      },
      "destroyed holding a closure");
}

// Single-thread model tests of the creator pool: every operation and boundary.

template <typename Pool>
std::vector<TaskRecord*> AllocateAll(Pool& pool, std::size_t count) {
  std::vector<TaskRecord*> records;
  for (std::size_t i = 0; i < count; ++i) {
    records.push_back(pool.tryAllocate());
  }
  return records;
}

TEST(CreatorRecordPoolTest, AllocatesUpToTheBoundThenSignalsInline) {
  CreatorRecordPool<> pool(4);
  const std::vector<TaskRecord*> records = AllocateAll(pool, 4);
  EXPECT_THAT(records, testing::Each(NotNull()));
  EXPECT_THAT(std::set<TaskRecord*>(records.begin(), records.end()).size(), Eq(4u));
  EXPECT_THAT(pool.liveCount(), Eq(4u));

  EXPECT_THAT(pool.tryAllocate(), IsNull()) << "past the bound the caller must run inline";
  EXPECT_THAT(pool.createdCount(), Eq(4u));

  for (TaskRecord* record : records) {
    pool.releaseFromCreator(record);
  }
  EXPECT_THAT(pool.liveCount(), Eq(0u));
}

TEST(CreatorRecordPoolTest, ARecordTheCreatorReleasesIsReusedWithoutANewOne) {
  CreatorRecordPool<> pool(2);
  TaskRecord* first = pool.tryAllocate();
  ASSERT_THAT(first, NotNull());
  pool.releaseFromCreator(first);

  EXPECT_THAT(pool.tryAllocate(), Eq(first));
  EXPECT_THAT(pool.createdCount(), Eq(1u));
  pool.releaseFromCreator(first);
}

TEST(CreatorRecordPoolTest, ReturnedRecordsStayLiveUntilTheCreatorTakesThemBack) {
  CreatorRecordPool<> pool(4);
  const std::vector<TaskRecord*> records = AllocateAll(pool, 4);
  for (TaskRecord* record : records) {
    pool.returnFromOtherThread(record);
  }
  EXPECT_THAT(pool.liveCount(), Eq(4u)) << "returned records count until the creator takes them";

  EXPECT_THAT(pool.reclaimReturned(), Eq(4u));
  EXPECT_THAT(pool.liveCount(), Eq(0u));
  EXPECT_THAT(pool.reclaimReturned(), Eq(0u));

  EXPECT_THAT(AllocateAll(pool, 4), UnorderedElementsAreArray(records));
  EXPECT_THAT(pool.createdCount(), Eq(4u)) << "returned records are reused, not replaced";
  for (TaskRecord* record : records) {
    pool.releaseFromCreator(record);
  }
}

TEST(CreatorRecordPoolTest, AnAllocationAtTheBoundTakesBackAReturnedRecord) {
  CreatorRecordPool<> pool(3);
  const std::vector<TaskRecord*> records = AllocateAll(pool, 3);
  ASSERT_THAT(pool.tryAllocate(), IsNull());

  pool.returnFromOtherThread(records[1]);
  EXPECT_THAT(pool.tryAllocate(), Eq(records[1]));
  EXPECT_THAT(pool.createdCount(), Eq(3u));
  for (TaskRecord* record : records) {
    pool.releaseFromCreator(record);
  }
}

TEST(CreatorRecordPoolTest, AnEmptyFreeListBelowTheBoundStillTakesReturnsFirst) {
  CreatorRecordPool<> pool(8);
  TaskRecord* record = pool.tryAllocate();
  pool.returnFromOtherThread(record);
  EXPECT_THAT(pool.tryAllocate(), Eq(record));
  EXPECT_THAT(pool.createdCount(), Eq(1u)) << "a returned record is preferred over a new one";
  pool.releaseFromCreator(record);
}

TEST(CreatorRecordPoolTest, AReusedRecordIsCleared) {
  CreatorRecordPool<> pool(1);
  std::atomic<std::uint64_t> counter{0};
  Parker<> parker;
  int calls = 0;

  TaskRecord* record = pool.tryAllocate();
  ASSERT_THAT(record, NotNull());
  EXPECT_THAT(record->pool(), Eq(&pool));
  record->setScope(&counter, &parker);
  record->setNext(record);
  record->setClosure([&] { ++calls; });
  record->run();
  pool.returnFromOtherThread(record);

  TaskRecord* reused = pool.tryAllocate();
  ASSERT_THAT(reused, Eq(record));
  EXPECT_THAT(reused->scopeCounter(), IsNull());
  EXPECT_THAT(reused->parker(), IsNull());
  EXPECT_THAT(reused->next(), IsNull());
  EXPECT_FALSE(reused->hasClosure());
  EXPECT_THAT(reused->pool(), Eq(&pool));
  EXPECT_THAT(calls, Eq(1));
  pool.releaseFromCreator(reused);
}

TEST(CreatorRecordPoolTest, DestructionTakesBackReturnedRecords) {
  // Leak-checked under AddressSanitizer: the destructor must delete records still on the return
  // stack.
  CreatorRecordPool<> pool(4);
  for (TaskRecord* record : AllocateAll(pool, 4)) {
    pool.returnFromOtherThread(record);
  }
}

TEST(CreatorRecordPoolDeathTest, MisuseFailsAReleaseAssertion) {
  EXPECT_DEATH({ CreatorRecordPool<> pool(0); }, "bound above zero");
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(1);
        TaskRecord* record = pool.tryAllocate();
        record->setClosure([] {});
        pool.releaseFromCreator(record);
      },
      "returned holding a closure");
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(1);
        TaskRecord* record = pool.tryAllocate();
        record->setClosure([] {});
        pool.returnFromOtherThread(record);
      },
      "returned holding a closure");
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(1);
        (void)pool.tryAllocate();
      },
      "destroyed with live records");
  EXPECT_DEATH(
      {
        CreatorRecordPool<> first(1);
        CreatorRecordPool<> second(1);
        second.returnFromOtherThread(first.tryAllocate());
      },
      "returned to another pool");
}

// In a release build the pools catch a creator re-releasing its current free head, any release at
// a live count of zero, a return of the record at the head of the return stack, and any take whose
// walk counts more records than are live; a double return while other records are still out can go
// unreported. AddressSanitizer builds catch every double return: the second one touches a poisoned
// record and fails there first.
TEST(CreatorRecordPoolDeathTest, ARecordReturnedTwiceFails) {
  constexpr const char* kReturnedTwice = "returned twice|use-after-poison";
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(2);
        TaskRecord* record = pool.tryAllocate();
        (void)pool.tryAllocate();
        pool.releaseFromCreator(record);
        pool.releaseFromCreator(record);
      },
      kReturnedTwice);
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(1);
        TaskRecord* record = pool.tryAllocate();
        pool.releaseFromCreator(record);
        pool.releaseFromCreator(record);
      },
      kReturnedTwice);
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(2);
        TaskRecord* record = pool.tryAllocate();
        pool.returnFromOtherThread(record);
        pool.returnFromOtherThread(record);
      },
      kReturnedTwice);
  // With another return between the two, the stack holds a cycle that the creator's take stops at.
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(2);
        TaskRecord* a = pool.tryAllocate();
        TaskRecord* b = pool.tryAllocate();
        pool.returnFromOtherThread(a);
        pool.returnFromOtherThread(b);
        pool.returnFromOtherThread(a);
        (void)pool.reclaimReturned();
      },
      "came back than were live|use-after-poison");
}

// The external pool: shared by every thread that is not a creator, under the guard mutex.

TEST(ExternalRecordPoolTest, AllocatesUpToTheBoundUnderTheGuard) {
  std::mutex guard;
  ExternalRecordPool pool(guard, 3);
  std::unique_lock lock(guard);
  std::vector<TaskRecord*> records;
  for (int i = 0; i < 3; ++i) {
    records.push_back(pool.tryAllocate(lock));
  }
  EXPECT_THAT(records, testing::Each(NotNull()));
  EXPECT_THAT(pool.tryAllocate(lock), IsNull()) << "past the bound the caller must run inline";
  EXPECT_THAT(pool.liveCount(lock), Eq(3u));

  pool.release(records[0], lock);
  EXPECT_THAT(pool.tryAllocate(lock), Eq(records[0]));
  for (TaskRecord* record : records) {
    pool.release(record, lock);
  }
  EXPECT_THAT(pool.liveCount(lock), Eq(0u));
}

TEST(ExternalRecordPoolDeathTest, UseWithoutTheGuardFailsAReleaseAssertion) {
  EXPECT_DEATH(
      {
        std::mutex guard;
        ExternalRecordPool pool(guard, 1);
        std::unique_lock<std::mutex> unlocked(guard, std::defer_lock);
        (void)pool.tryAllocate(unlocked);
      },
      "without its guard mutex");
  EXPECT_DEATH(
      {
        std::mutex guard;
        std::mutex other;
        ExternalRecordPool pool(guard, 1);
        std::unique_lock lock(other);
        (void)pool.tryAllocate(lock);
      },
      "without its guard mutex");
  EXPECT_DEATH(
      {
        std::mutex guard;
        ExternalRecordPool pool(guard, 1);
        std::unique_lock lock(guard);
        (void)pool.tryAllocate(lock);
        lock.unlock();
      },
      "destroyed with live records");
}

// Use after return: a free record is poisoned, so AddressSanitizer stops a thread that touches it.

#if defined(DONNER_TASKS_RECORD_ASAN)
constexpr bool kAddressSanitizer = true;
#else
constexpr bool kAddressSanitizer = false;
#endif

/// Whether the byte \p offset bytes into \p record is poisoned.
bool PoisonedAt(const TaskRecord* record, std::size_t offset) {
#if defined(DONNER_TASKS_RECORD_ASAN)
  return __asan_address_is_poisoned(reinterpret_cast<const char*>(record) + offset) != 0;
#else
  (void)record;
  (void)offset;
  return false;
#endif
}

/// Offsets of the first byte after the link, of the inline closure storage, and of the last byte.
constexpr std::array<std::size_t, 3> kPayloadOffsets = {sizeof(TaskRecord*), 64,
                                                        sizeof(TaskRecord) - 1};

TEST(TaskRecordPoisonTest, AFreeRecordIsPoisonedUntilItIsAllocatedAgain) {
  if (!kAddressSanitizer) {
    GTEST_SKIP() << "free records are poisoned only in AddressSanitizer builds";
  }
  CreatorRecordPool<> pool(2);
  TaskRecord* ownRecord = pool.tryAllocate();
  TaskRecord* stolenRecord = pool.tryAllocate();
  for (std::size_t offset : kPayloadOffsets) {
    EXPECT_FALSE(PoisonedAt(ownRecord, offset)) << "live record, offset " << offset;
  }

  pool.releaseFromCreator(ownRecord);
  pool.returnFromOtherThread(stolenRecord);
  for (std::size_t offset : kPayloadOffsets) {
    EXPECT_TRUE(PoisonedAt(ownRecord, offset)) << "released record, offset " << offset;
    EXPECT_TRUE(PoisonedAt(stolenRecord, offset)) << "returned record, offset " << offset;
  }
  EXPECT_FALSE(PoisonedAt(ownRecord, 0)) << "the link stays usable on a free list";

  EXPECT_THAT(AllocateAll(pool, 2), UnorderedElementsAreArray({ownRecord, stolenRecord}));
  for (std::size_t offset : kPayloadOffsets) {
    EXPECT_FALSE(PoisonedAt(ownRecord, offset)) << "reallocated record, offset " << offset;
    EXPECT_FALSE(PoisonedAt(stolenRecord, offset)) << "reallocated record, offset " << offset;
  }
  pool.releaseFromCreator(ownRecord);
  pool.releaseFromCreator(stolenRecord);
}

TEST(TaskRecordPoisonDeathTest, TouchingARecordAfterReturningItFails) {
  if (!kAddressSanitizer) {
    GTEST_SKIP() << "free records are poisoned only in AddressSanitizer builds";
  }
  EXPECT_DEATH(
      {
        CreatorRecordPool<> pool(1);
        TaskRecord* record = pool.tryAllocate();
        pool.returnFromOtherThread(record);
        std::atomic<std::uint64_t> counter{0};
        record->setScope(&counter, nullptr);
      },
      "use-after-poison");
}

// Deterministic interleavings of a return with the creator's take of the return stack, with one
// thread held inside a pool hook while another runs.

/// A rendezvous that holds one thread in a hook until the test releases it.
class Gate {
public:
  void arriveAndWait() {
    std::unique_lock lock(mutex_);
    arrived_ = true;
    cv_.notify_all();
    if (!cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                        [this] { return released_; })) {
      heldPastHangGuard_ = true;
    }
  }

  [[nodiscard]] bool waitForArrival() {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [this] { return arrived_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  [[nodiscard]] bool heldPastHangGuard() {
    std::lock_guard lock(mutex_);
    return heldPastHangGuard_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool arrived_ = false;
  bool released_ = false;
  bool heldPastHangGuard_ = false;
};

/// Hooks that hold the next return publish, or the next take, at a \ref Gate when asked.
struct InterleavingHooks {
  static inline std::atomic<bool> holdNextPublish{false};
  static inline std::atomic<bool> holdNextTake{false};
  static inline Gate* publishGate = nullptr;
  static inline Gate* takeGate = nullptr;

  static void BeforeReturnPublishes() {
    if (holdNextPublish.exchange(false)) {
      publishGate->arriveAndWait();
    }
  }

  static void BeforeOwnerTakesReturns() {
    if (holdNextTake.exchange(false)) {
      takeGate->arriveAndWait();
    }
  }
};

/// Releases both gates and joins a case's threads when the case ends by any path.
class ReleaseAndJoin {
public:
  ReleaseAndJoin(Gate& a, Gate& b, std::initializer_list<std::thread*> threads)
      : a_(a), b_(b), threads_(threads) {}

  ReleaseAndJoin(const ReleaseAndJoin&) = delete;
  ReleaseAndJoin& operator=(const ReleaseAndJoin&) = delete;

  ~ReleaseAndJoin() {
    a_.release();
    b_.release();
    for (std::thread* thread : threads_) {
      if (thread->joinable()) {
        thread->join();
      }
    }
  }

private:
  Gate& a_;
  Gate& b_;
  std::vector<std::thread*> threads_;
};

class ReturnStackInterleavingTest : public testing::Test {
protected:
  using Pool = CreatorRecordPool<InterleavingHooks>;

  void SetUp() override {
    InterleavingHooks::publishGate = &publishGate_;
    InterleavingHooks::takeGate = &takeGate_;
  }

  void TearDown() override {
    InterleavingHooks::holdNextPublish = false;
    InterleavingHooks::holdNextTake = false;
    InterleavingHooks::publishGate = nullptr;
    InterleavingHooks::takeGate = nullptr;
  }

  Gate publishGate_;
  Gate takeGate_;
};

// A return lands while the creator is held between seeing returned records and taking them: its
// exchange takes both.
TEST_F(ReturnStackInterleavingTest, AReturnDuringTheCreatorsTakeIsTakenWithIt) {
  Pool pool(2);
  const std::vector<TaskRecord*> records = AllocateAll(pool, 2);
  pool.returnFromOtherThread(records[0]);

  std::size_t taken = 0;
  std::thread creator;
  ReleaseAndJoin joinOnExit(publishGate_, takeGate_, {&creator});
  InterleavingHooks::holdNextTake = true;
  creator = std::thread([&] { taken = pool.reclaimReturned(); });
  ASSERT_TRUE(takeGate_.waitForArrival());

  std::thread returner([&] { pool.returnFromOtherThread(records[1]); });
  returner.join();

  takeGate_.release();
  creator.join();
  EXPECT_THAT(taken, Eq(2u));
  EXPECT_THAT(pool.liveCount(), Eq(0u));
  EXPECT_THAT(AllocateAll(pool, 2), UnorderedElementsAreArray(records));
  EXPECT_THAT(pool.createdCount(), Eq(2u));
  EXPECT_FALSE(takeGate_.heldPastHangGuard());
  for (TaskRecord* record : records) {
    pool.releaseFromCreator(record);
  }
}

// A return is held after reading an empty head, so the creator's take finds nothing and the
// creator, at its bound, must run inline; once the return publishes, the next allocation gets it.
TEST_F(ReturnStackInterleavingTest, AReturnNotYetPublishedLandsInTheNextTake) {
  Pool pool(1);
  TaskRecord* record = pool.tryAllocate();

  std::thread returner;
  ReleaseAndJoin joinOnExit(publishGate_, takeGate_, {&returner});
  InterleavingHooks::holdNextPublish = true;
  returner = std::thread([&] { pool.returnFromOtherThread(record); });
  ASSERT_TRUE(publishGate_.waitForArrival());

  EXPECT_THAT(pool.tryAllocate(), IsNull()) << "the unpublished return is not the creator's yet";
  EXPECT_THAT(pool.liveCount(), Eq(1u));

  publishGate_.release();
  returner.join();
  EXPECT_THAT(pool.tryAllocate(), Eq(record));
  EXPECT_THAT(pool.createdCount(), Eq(1u));
  EXPECT_FALSE(publishGate_.heldPastHangGuard());
  pool.releaseFromCreator(record);
}

// A return held before its publish while another return lands: the held compare-and-swap fails
// on the changed head and retries, so neither record is lost.
TEST_F(ReturnStackInterleavingTest, TwoRacingReturnsAreBothKept) {
  Pool pool(2);
  const std::vector<TaskRecord*> records = AllocateAll(pool, 2);

  std::thread held;
  ReleaseAndJoin joinOnExit(publishGate_, takeGate_, {&held});
  InterleavingHooks::holdNextPublish = true;
  held = std::thread([&] { pool.returnFromOtherThread(records[0]); });
  ASSERT_TRUE(publishGate_.waitForArrival());

  std::thread other([&] { pool.returnFromOtherThread(records[1]); });
  other.join();

  publishGate_.release();
  held.join();
  EXPECT_THAT(pool.reclaimReturned(), Eq(2u));
  EXPECT_THAT(AllocateAll(pool, 2), UnorderedElementsAreArray(records));
  EXPECT_THAT(pool.createdCount(), Eq(2u));
  EXPECT_FALSE(publishGate_.heldPastHangGuard());
  for (TaskRecord* record : records) {
    pool.releaseFromCreator(record);
  }
}

// ABA: a return reads head A and is held; meanwhile the creator takes A, reuses it, and A is
// returned again, so the head is A once more. The held compare-and-swap then succeeds against the
// same pointer, and since it links to A as A now stands, both records are kept exactly once.
TEST_F(ReturnStackInterleavingTest, AHeadTakenReusedAndReturnedAgainIsNotCorrupted) {
  Pool pool(2);
  const std::vector<TaskRecord*> records = AllocateAll(pool, 2);
  TaskRecord* a = records[0];
  TaskRecord* b = records[1];
  pool.returnFromOtherThread(a);

  std::thread held;
  ReleaseAndJoin joinOnExit(publishGate_, takeGate_, {&held});
  InterleavingHooks::holdNextPublish = true;
  held = std::thread([&] { pool.returnFromOtherThread(b); });
  ASSERT_TRUE(publishGate_.waitForArrival());

  ASSERT_THAT(pool.reclaimReturned(), Eq(1u));
  ASSERT_THAT(pool.tryAllocate(), Eq(a)) << "the creator reuses the record it took back";
  int calls = 0;
  a->setClosure([&] { ++calls; });
  std::thread runner([&] {
    a->run();
    pool.returnFromOtherThread(a);
  });
  runner.join();

  publishGate_.release();
  held.join();
  EXPECT_THAT(calls, Eq(1));
  EXPECT_THAT(pool.reclaimReturned(), Eq(2u));
  EXPECT_THAT(pool.liveCount(), Eq(0u));
  EXPECT_THAT(AllocateAll(pool, 2), UnorderedElementsAreArray(records));
  EXPECT_THAT(pool.tryAllocate(), IsNull()) << "each record must come back exactly once";
  EXPECT_THAT(pool.createdCount(), Eq(2u));
  EXPECT_FALSE(publishGate_.heldPastHangGuard());
  for (TaskRecord* record : records) {
    pool.releaseFromCreator(record);
  }
}

// Multi-thread stress: the creator hands records to several runners, which return them from their
// own threads while the creator allocates again.

/// Small deterministic generator; one per thread.
class XorShift32 {
public:
  explicit XorShift32(std::uint32_t seed) : state_(seed == 0 ? 1 : seed) {}

  std::uint32_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
  }

private:
  std::uint32_t state_;
};

/// Pool hooks that delay a return between linking and publishing, and the creator between seeing
/// returns and taking them: usually a yield, sometimes a few microseconds.
struct DelayedPoolHooks {
  static void BeforeReturnPublishes() { Delay(); }
  static void BeforeOwnerTakesReturns() { Delay(); }

  static void Delay() {
    thread_local XorShift32 rng(
        static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    const std::uint32_t roll = rng.next() % 16;
    if (roll < 8) {
      std::this_thread::yield();
    } else if (roll < 10) {
      std::this_thread::sleep_for(std::chrono::microseconds(roll));
    }
  }
};

struct PlainPoolHooksName {
  using Hooks = NoRecordPoolHooks;
  static constexpr int kTasks = 100000;
};
struct DelayedPoolHooksName {
  using Hooks = DelayedPoolHooks;
  static constexpr int kTasks = 20000;
};

using PoolHookForms = testing::Types<PlainPoolHooksName, DelayedPoolHooksName>;

class PoolHookFormNames {
public:
  template <typename T>
  static std::string GetName(int /*index*/) {
    return std::is_same_v<T, PlainPoolHooksName> ? "NoHooks" : "InjectedDelay";
  }
};

/// The checks a cross-thread return run makes, collected so a failure reports them all.
struct ReturnDefects {
  std::int64_t tasksNotRunOnce = 0;
  std::int64_t recordsHandedOutTwice = 0;
  std::int64_t recordsWithAStaleClosure = 0;
  std::int64_t liveAfterReclaim = 0;
  bool createdPastTheBound = false;
  bool stalled = false;

  friend bool operator==(const ReturnDefects&, const ReturnDefects&) = default;

  friend std::ostream& operator<<(std::ostream& os, const ReturnDefects& defects) {
    return os << "{tasksNotRunOnce=" << defects.tasksNotRunOnce
              << " recordsHandedOutTwice=" << defects.recordsHandedOutTwice
              << " recordsWithAStaleClosure=" << defects.recordsWithAStaleClosure
              << " liveAfterReclaim=" << defects.liveAfterReclaim
              << " createdPastTheBound=" << defects.createdPastTheBound
              << " stalled=" << defects.stalled << "}";
  }
};

template <typename NameT>
class CreatorRecordPoolStressTest : public testing::Test {
protected:
  using Pool = CreatorRecordPool<typename NameT::Hooks>;
  static constexpr int kTasks = NameT::kTasks;
  static constexpr int kRunners = 4;
  static constexpr std::size_t kBound = 8;
};

TYPED_TEST_SUITE(CreatorRecordPoolStressTest, PoolHookForms, PoolHookFormNames);

// The creator hands records to runners through one ring per runner, whose slots it fills with a
// release store and the runner empties with a relaxed one, so nothing but the pool's return stack
// orders a runner's last writes to a record before the creator reuses it. The closure the creator
// stores and the runner destroys lives in the record, so a return that does not publish those
// writes is a race ThreadSanitizer reports, or a stale closure the creator finds on reuse. In-use
// flags are relaxed for the same reason. Each round the creator allocates until the pool signals
// the bound, runs that one task inline, and hands the records out, so the bound is reached every
// round while returns race the creator's takes, every record is reused thousands of times, and a
// head that is taken, reused and returned again while another return holds it is routine.
TYPED_TEST(CreatorRecordPoolStressTest, CrossThreadReturnsKeepEveryRecordAndRunEveryTaskOnce) {
  using Pool = typename TestFixture::Pool;
  constexpr int kTasks = TestFixture::kTasks;
  constexpr int kRunners = TestFixture::kRunners;
  constexpr std::size_t kBound = TestFixture::kBound;
  Pool pool(kBound);

  // Create every record up front, so the in-use table is fixed before the runners start.
  std::map<const TaskRecord*, std::atomic<bool>> inUse;
  for (TaskRecord* record : AllocateAll(pool, kBound)) {
    inUse[record].store(false);
    pool.releaseFromCreator(record);
  }

  std::vector<int> runCounts(kTasks, 0);
  std::array<std::array<std::atomic<TaskRecord*>, kBound>, kRunners> rings{};
  std::atomic<bool> creatorDone{false};
  std::atomic<std::int64_t> handedOutTwice{0};
  ReturnDefects defects;
  int rounds = 0;
  int inlineRuns = 0;

  std::vector<std::thread> runners;
  for (int i = 0; i < kRunners; ++i) {
    runners.emplace_back([&, i] {
      std::array<std::atomic<TaskRecord*>, kBound>& ring = rings[static_cast<std::size_t>(i)];
      std::size_t next = 0;
      while (true) {
        TaskRecord* record = ring[next].load(std::memory_order_acquire);
        if (record == nullptr) {
          if (creatorDone.load(std::memory_order_acquire) &&
              ring[next].load(std::memory_order_acquire) == nullptr) {
            return;
          }
          std::this_thread::yield();
          continue;
        }
        ring[next].store(nullptr, std::memory_order_relaxed);
        next = (next + 1) % kBound;
        record->run();
        inUse.at(record).store(false, std::memory_order_relaxed);
        pool.returnFromOtherThread(record);
      }
    });
  }

  const tests::HangGuard guard;
  std::array<std::size_t, kRunners> ringPositions{};
  std::size_t nextRunner = 0;
  std::vector<TaskRecord*> batch;
  int task = 0;
  while (task < kTasks && guard.active()) {
    ++rounds;
    batch.clear();
    while (task < kTasks) {
      TaskRecord* record = pool.tryAllocate();
      if (pool.createdCount() > kBound || pool.liveCount() > kBound) {
        defects.createdPastTheBound = true;
      }
      if (record == nullptr) {
        ++inlineRuns;
        ++runCounts[static_cast<std::size_t>(task++)];
        break;
      }
      if (inUse.at(record).exchange(true, std::memory_order_relaxed)) {
        handedOutTwice.fetch_add(1, std::memory_order_relaxed);
      }
      if (record->hasClosure()) {
        ++defects.recordsWithAStaleClosure;
        record->discard();
      }
      int* runCount = &runCounts[static_cast<std::size_t>(task++)];
      record->setClosure([runCount] { ++*runCount; });
      batch.push_back(record);
    }
    for (TaskRecord* record : batch) {
      std::atomic<TaskRecord*>& slot = rings[nextRunner][ringPositions[nextRunner]];
      while (slot.load(std::memory_order_relaxed) != nullptr && guard.active()) {
        std::this_thread::yield();
      }
      slot.store(record, std::memory_order_release);
      ringPositions[nextRunner] = (ringPositions[nextRunner] + 1) % kBound;
      nextRunner = (nextRunner + 1) % kRunners;
    }
  }
  creatorDone.store(true, std::memory_order_release);
  for (std::thread& runner : runners) {
    runner.join();
  }
  defects.stalled = !guard.active();

  pool.reclaimReturned();
  defects.liveAfterReclaim = static_cast<std::int64_t>(pool.liveCount());
  defects.recordsHandedOutTwice = handedOutTwice.load();
  for (int count : runCounts) {
    if (count != 1) {
      ++defects.tasksNotRunOnce;
    }
  }
  EXPECT_THAT(defects, Eq(ReturnDefects{}));
  // Every round but the last, which may end when the tasks run out, ends at the bound.
  EXPECT_THAT(inlineRuns, AllOf(Gt(0), Ge(rounds - 1)));
  EXPECT_THAT(pool.createdCount(), Le(kBound));
}

// Several threads allocate and release from the external pool under its guard. The number of
// records out at once never exceeds the bound, and with more threads than the bound some
// allocations must find it reached.
TEST(ExternalRecordPoolStressTest, TheBoundHoldsUnderContention) {
  constexpr int kThreads = 6;
  constexpr int kAllocationsPerThread = 20000;
  constexpr std::size_t kBound = 3;
  std::mutex guard;
  ExternalRecordPool pool(guard, kBound);
  std::atomic<std::int64_t> out{0};
  std::atomic<std::int64_t> mostOut{0};
  std::atomic<std::int64_t> boundHits{0};
  std::atomic<std::int64_t> runs{0};

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < kAllocationsPerThread; ++j) {
        TaskRecord* record = nullptr;
        {
          std::unique_lock lock(guard);
          record = pool.tryAllocate(lock);
        }
        if (record == nullptr) {
          boundHits.fetch_add(1, std::memory_order_relaxed);
          runs.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const std::int64_t nowOut = out.fetch_add(1, std::memory_order_relaxed) + 1;
        std::int64_t seen = mostOut.load(std::memory_order_relaxed);
        while (nowOut > seen &&
               !mostOut.compare_exchange_weak(seen, nowOut, std::memory_order_relaxed)) {}
        record->setClosure([&] { runs.fetch_add(1, std::memory_order_relaxed); });
        record->run();
        out.fetch_sub(1, std::memory_order_relaxed);
        std::unique_lock lock(guard);
        pool.release(record, lock);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_THAT(mostOut.load(), Le(static_cast<std::int64_t>(kBound)));
  EXPECT_THAT(boundHits.load(), Gt(0));
  EXPECT_THAT(runs.load(), Eq(kThreads * kAllocationsPerThread));
  std::unique_lock lock(guard);
  EXPECT_THAT(pool.liveCount(lock), Eq(0u));
}

}  // namespace
}  // namespace donner::tasks
