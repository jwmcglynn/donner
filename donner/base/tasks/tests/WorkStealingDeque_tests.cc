#include "donner/base/tasks/WorkStealingDeque.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <latch>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {
namespace {

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

/// Names a deque ordering form as a gtest type parameter.
template <DequeOrdering kOrdering>
struct Form {
  static constexpr DequeOrdering kValue = kOrdering;
};

using Forms =
    testing::Types<Form<DequeOrdering::WeakMemory>, Form<DequeOrdering::SequentiallyConsistent>>;

class FormNames {
public:
  template <typename T>
  static std::string GetName(int /*index*/) {
    std::ostringstream os;
    os << T::kValue;
    return os.str();
  }
};

template <typename T>
StealResult<T> Stolen(T item) {
  return StealResult<T>{StealStatus::Taken, item};
}

template <typename T>
StealResult<T> NothingStolen(StealStatus status) {
  return StealResult<T>{status, T{}};
}

/// Hooks that count how often each claim path runs.
struct CountingHooks {
  static inline std::atomic<int> takeClaims{0};
  static inline std::atomic<int> stealClaims{0};

  static void BeforeTakeClaimsLastEntry() { takeClaims.fetch_add(1); }
  static void BeforeStealClaims() { stealClaims.fetch_add(1); }

  static void Reset() {
    takeClaims = 0;
    stealClaims = 0;
  }
};

// Single-thread model tests: every operation and boundary of one deque.

template <typename FormT>
class WorkStealingDequeModelTest : public testing::Test {
protected:
  using Deque = WorkStealingDeque<int, FormT::kValue, CountingHooks>;

  void SetUp() override { CountingHooks::Reset(); }
};

TYPED_TEST_SUITE(WorkStealingDequeModelTest, Forms, FormNames);

TYPED_TEST(WorkStealingDequeModelTest, NewDequeIsEmptyAndTakeOnEmptyRestoresTheBottom) {
  typename TestFixture::Deque deque(4);
  EXPECT_THAT(deque.capacity(), Eq(4u));

  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(deque.take(), Eq(std::nullopt));
    EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));
  }

  ASSERT_TRUE(deque.push(7));
  EXPECT_THAT(deque.take(), Optional(7));
  EXPECT_THAT(deque.take(), Eq(std::nullopt));
}

TYPED_TEST(WorkStealingDequeModelTest, OneEntryIsTakenThroughTheLastEntryClaim) {
  typename TestFixture::Deque deque(4);
  ASSERT_TRUE(deque.push(5));

  EXPECT_THAT(deque.take(), Optional(5));
  EXPECT_THAT(CountingHooks::takeClaims.load(), Eq(1));

  EXPECT_THAT(deque.take(), Eq(std::nullopt));
  EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));
  EXPECT_THAT(CountingHooks::takeClaims.load(), Eq(1));
}

TYPED_TEST(WorkStealingDequeModelTest, OneEntryIsStolenThroughTheTopClaim) {
  typename TestFixture::Deque deque(4);
  ASSERT_TRUE(deque.push(5));

  EXPECT_THAT(deque.steal(), Eq(Stolen(5)));
  EXPECT_THAT(CountingHooks::stealClaims.load(), Eq(1));

  EXPECT_THAT(deque.take(), Eq(std::nullopt));
  EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));
}

TYPED_TEST(WorkStealingDequeModelTest, TakeReturnsTheNewestAndStealTheOldest) {
  typename TestFixture::Deque deque(8);
  for (int i = 1; i <= 4; ++i) {
    ASSERT_TRUE(deque.push(i));
  }

  EXPECT_THAT(deque.steal(), Eq(Stolen(1)));
  EXPECT_THAT(deque.take(), Optional(4));
  EXPECT_THAT(deque.steal(), Eq(Stolen(2)));
  EXPECT_THAT(deque.take(), Optional(3));
  EXPECT_THAT(deque.take(), Eq(std::nullopt));
  EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));

  // Only the take of 3 found a single entry; the take of 4 had two below it.
  EXPECT_THAT(CountingHooks::takeClaims.load(), Eq(1));
}

TYPED_TEST(WorkStealingDequeModelTest, FullDequeRejectsPushesUntilAnEntryLeaves) {
  typename TestFixture::Deque deque(4);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(deque.push(i));
  }
  EXPECT_FALSE(deque.push(4)) << "a push past the capacity must fail";

  EXPECT_THAT(deque.steal(), Eq(Stolen(0)));
  EXPECT_TRUE(deque.push(4)) << "a steal frees one slot";
  EXPECT_FALSE(deque.push(5));

  EXPECT_THAT(deque.take(), Optional(4));
  EXPECT_TRUE(deque.push(5)) << "a take frees one slot";
  EXPECT_FALSE(deque.push(6));

  // Contents, oldest first: 1, 2, 3, 5. The failed pushes left them unchanged.
  std::vector<int> drained;
  drained.push_back(deque.take().value_or(-1));
  drained.push_back(deque.steal().item);
  drained.push_back(deque.take().value_or(-1));
  drained.push_back(deque.steal().item);
  EXPECT_THAT(drained, ElementsAre(5, 1, 3, 2));
  EXPECT_THAT(deque.take(), Eq(std::nullopt));
}

TYPED_TEST(WorkStealingDequeModelTest, CapacityOneHoldsOneEntry) {
  typename TestFixture::Deque deque(1);
  ASSERT_TRUE(deque.push(1));
  EXPECT_FALSE(deque.push(2));
  EXPECT_THAT(deque.take(), Optional(1));
  ASSERT_TRUE(deque.push(2));
  EXPECT_FALSE(deque.push(3));
  EXPECT_THAT(deque.steal(), Eq(Stolen(2)));
  ASSERT_TRUE(deque.push(3));
  EXPECT_THAT(deque.take(), Optional(3));
  EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));
}

TYPED_TEST(WorkStealingDequeModelTest, LogicalIndicesPastTheCapacityWrapAroundTheRing) {
  typename TestFixture::Deque deque(4);
  // Each round advances the top by one and leaves the deque empty, so after 1000 rounds both
  // indices are about 1000 ring lengths in, and every slot has been reused many times.
  for (int round = 0; round < 1000; ++round) {
    const int base = round * 3;
    ASSERT_TRUE(deque.push(base));
    ASSERT_TRUE(deque.push(base + 1));
    ASSERT_TRUE(deque.push(base + 2));
    ASSERT_THAT(deque.steal(), Eq(Stolen(base))) << "round " << round;
    ASSERT_THAT(deque.take(), Optional(base + 2)) << "round " << round;
    ASSERT_THAT(deque.take(), Optional(base + 1)) << "round " << round;
    ASSERT_THAT(deque.take(), Eq(std::nullopt)) << "round " << round;
  }
  EXPECT_THAT(CountingHooks::takeClaims.load(), Eq(1000));
}

TYPED_TEST(WorkStealingDequeModelTest, RandomOperationsMatchAReferenceDeque) {
  constexpr std::size_t kCapacity = 8;
  typename TestFixture::Deque deque(kCapacity);
  std::deque<int> model;
  std::mt19937 rng(20261123);
  std::uniform_int_distribution<int> pickOp(0, 2);

  int nextValue = 0;
  for (int op = 0; op < 100000; ++op) {
    switch (pickOp(rng)) {
      case 0: {
        const bool modelAccepts = model.size() < kCapacity;
        ASSERT_THAT(deque.push(nextValue), Eq(modelAccepts)) << "push at op " << op;
        if (modelAccepts) {
          model.push_back(nextValue);
        }
        ++nextValue;
        break;
      }
      case 1: {
        std::optional<int> expected;
        if (!model.empty()) {
          expected = model.back();
          model.pop_back();
        }
        ASSERT_THAT(deque.take(), Eq(expected)) << "take at op " << op;
        break;
      }
      default: {
        StealResult<int> expected = NothingStolen<int>(StealStatus::Empty);
        if (!model.empty()) {
          expected = Stolen(model.front());
          model.pop_front();
        }
        ASSERT_THAT(deque.steal(), Eq(expected)) << "steal at op " << op;
        break;
      }
    }
  }
}

TEST(WorkStealingDequeDeathTest, CapacityMustBeAPowerOfTwoWithinTheLimit) {
  EXPECT_DEATH({ WorkStealingDeque<int> deque(0); }, "power of two");
  EXPECT_DEATH({ WorkStealingDeque<int> deque(3); }, "power of two");
  EXPECT_DEATH({ WorkStealingDeque<int> deque(48); }, "power of two");
  EXPECT_DEATH(
      { WorkStealingDeque<int> deque(WorkStealingDeque<int>::kMaxCapacity * 2); }, "kMaxCapacity");
}

// Deterministic interleavings of the race on the last entry, with one thread held inside a claim
// hook while another runs.

/// A rendezvous that holds one thread in a hook until the test releases it.
class Gate {
public:
  /// Called by the held thread: reports arrival, then waits for \ref release.
  void arriveAndWait() {
    std::unique_lock lock(mutex_);
    arrived_ = true;
    cv_.notify_all();
    if (!cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                        [this] { return released_; })) {
      heldPastHangGuard_ = true;
    }
  }

  /// Waits until a thread arrives. @return False if none arrived within the hang guard.
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

  /// Whether a held thread gave up waiting for \ref release.
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

/// Hooks that hold the next claim of each kind at a \ref Gate when the test asks for it.
struct InterleavingHooks {
  static inline std::atomic<bool> holdNextTakeClaim{false};
  static inline std::atomic<bool> holdNextStealClaim{false};
  static inline Gate* takeGate = nullptr;
  static inline Gate* stealGate = nullptr;

  static void BeforeTakeClaimsLastEntry() {
    if (holdNextTakeClaim.exchange(false)) {
      takeGate->arriveAndWait();
    }
  }

  static void BeforeStealClaims() {
    if (holdNextStealClaim.exchange(false)) {
      stealGate->arriveAndWait();
    }
  }
};

template <typename FormT>
class WorkStealingDequeInterleavingTest : public testing::Test {
protected:
  using Deque = WorkStealingDeque<int, FormT::kValue, InterleavingHooks>;

  void SetUp() override {
    InterleavingHooks::takeGate = &takeGate_;
    InterleavingHooks::stealGate = &stealGate_;
  }

  void TearDown() override {
    InterleavingHooks::holdNextTakeClaim = false;
    InterleavingHooks::holdNextStealClaim = false;
    InterleavingHooks::takeGate = nullptr;
    InterleavingHooks::stealGate = nullptr;
  }

  Gate takeGate_;
  Gate stealGate_;
};

TYPED_TEST_SUITE(WorkStealingDequeInterleavingTest, Forms, FormNames);

// The thief has read the last entry and is held before its claim; the owner takes it meanwhile.
TYPED_TEST(WorkStealingDequeInterleavingTest, OwnerClaimsTheLastEntryBeforeAHeldThief) {
  typename TestFixture::Deque deque(4);
  ASSERT_TRUE(deque.push(42));

  InterleavingHooks::holdNextStealClaim = true;
  StealResult<int> thiefResult;
  std::thread thief([&] { thiefResult = deque.steal(); });
  ASSERT_TRUE(this->stealGate_.waitForArrival());

  EXPECT_THAT(deque.take(), Optional(42));

  this->stealGate_.release();
  thief.join();
  EXPECT_THAT(thiefResult, Eq(NothingStolen<int>(StealStatus::Lost)))
      << "the owner claimed the entry, so the thief's claim must fail";
  EXPECT_FALSE(this->stealGate_.heldPastHangGuard());

  EXPECT_THAT(deque.take(), Eq(std::nullopt));
  EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));
}

// The owner has lowered the bottom and is held before its claim; a thief that read the bottom
// before it was lowered claims the entry meanwhile.
TYPED_TEST(WorkStealingDequeInterleavingTest, HeldThiefClaimsTheLastEntryBeforeAHeldOwner) {
  typename TestFixture::Deque deque(4);
  ASSERT_TRUE(deque.push(42));

  InterleavingHooks::holdNextStealClaim = true;
  StealResult<int> thiefResult;
  std::thread thief([&] { thiefResult = deque.steal(); });
  ASSERT_TRUE(this->stealGate_.waitForArrival());

  InterleavingHooks::holdNextTakeClaim = true;
  std::optional<int> ownerResult;
  std::thread owner([&] { ownerResult = deque.take(); });
  ASSERT_TRUE(this->takeGate_.waitForArrival());

  this->stealGate_.release();
  thief.join();
  EXPECT_THAT(thiefResult, Eq(Stolen(42)));

  this->takeGate_.release();
  owner.join();
  EXPECT_THAT(ownerResult, Eq(std::nullopt))
      << "the thief claimed the entry, so the owner's claim must fail";
  EXPECT_FALSE(this->stealGate_.heldPastHangGuard());
  EXPECT_FALSE(this->takeGate_.heldPastHangGuard());

  // The owner's failed claim restored the bottom to the top, so the deque is empty and usable.
  EXPECT_THAT(deque.steal(), Eq(NothingStolen<int>(StealStatus::Empty)));
  ASSERT_TRUE(deque.push(43));
  EXPECT_THAT(deque.take(), Optional(43));
}

// Two thieves read the same oldest entry; only the first claim succeeds.
TYPED_TEST(WorkStealingDequeInterleavingTest, OnlyOneOfTwoThievesClaimsTheSameEntry) {
  typename TestFixture::Deque deque(4);
  ASSERT_TRUE(deque.push(1));
  ASSERT_TRUE(deque.push(2));

  InterleavingHooks::holdNextStealClaim = true;
  StealResult<int> heldResult;
  std::thread heldThief([&] { heldResult = deque.steal(); });
  ASSERT_TRUE(this->stealGate_.waitForArrival());

  StealResult<int> otherResult;
  std::thread otherThief([&] { otherResult = deque.steal(); });
  otherThief.join();
  EXPECT_THAT(otherResult, Eq(Stolen(1)));

  this->stealGate_.release();
  heldThief.join();
  EXPECT_THAT(heldResult, Eq(NothingStolen<int>(StealStatus::Lost)));
  EXPECT_FALSE(this->stealGate_.heldPastHangGuard());

  EXPECT_THAT(deque.steal(), Eq(Stolen(2)));
}

// Multi-thread stress: one owner, several thieves, every pushed entry taken exactly once.

/// An entry's record. The owner writes it with plain stores before pushing its pointer, so a
/// taker that reads it checks that the deque published it.
struct Record {
  std::int64_t id = 0;
  std::uint64_t payload = 0;
};

std::uint64_t PayloadFor(std::int64_t id) {
  return static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ull + 1;
}

/// What one thread took.
struct TakerLog {
  std::vector<std::int64_t> ids;
  std::int64_t payloadMismatches = 0;
  std::int64_t nullEntries = 0;

  void record(const Record* record) {
    if (record == nullptr) {
      ++nullEntries;
      return;
    }
    ids.push_back(record->id);
    if (record->payload != PayloadFor(record->id)) {
      ++payloadMismatches;
    }
  }
};

/// The checks every stress run makes, collected so a failure reports them all.
struct StressDefects {
  std::int64_t entriesNotTakenOnce = 0;
  std::vector<std::int64_t> firstEntriesNotTakenOnce;
  std::int64_t payloadMismatches = 0;
  std::int64_t nullEntries = 0;
  std::int64_t stealsOutOfOrder = 0;

  friend bool operator==(const StressDefects&, const StressDefects&) = default;

  friend std::ostream& operator<<(std::ostream& os, const StressDefects& defects) {
    os << "{entriesNotTakenOnce=" << defects.entriesNotTakenOnce << " first=[";
    for (std::int64_t id : defects.firstEntriesNotTakenOnce) {
      os << id << " ";
    }
    return os << "] payloadMismatches=" << defects.payloadMismatches
              << " nullEntries=" << defects.nullEntries
              << " stealsOutOfOrder=" << defects.stealsOutOfOrder << "}";
  }
};

/// Small deterministic generator for the owner's choices; no shared state with the deque.
class XorShift64 {
public:
  explicit XorShift64(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

private:
  std::uint64_t state_;
};

template <typename FormT>
class WorkStealingDequeStressTest : public testing::Test {
protected:
  using Deque = WorkStealingDeque<Record*, FormT::kValue>;

  static constexpr int kThieves = 3;

  /**
   * Runs one owner against \ref kThieves thieves and checks the outcome.
   *
   * @param entries Number of entries the owner pushes.
   * @param capacity Deque capacity.
   * @param ownerStep Called once per entry on the owner thread with the deque, the entry and the
   *   owner's log; it must push the entry, taking entries itself whenever the deque is full.
   */
  template <typename OwnerStep>
  StressDefects run(std::int64_t entries, std::size_t capacity, OwnerStep ownerStep) {
    std::vector<Record> records(static_cast<std::size_t>(entries));
    Deque deque(capacity);
    std::atomic<bool> ownerDone{false};
    std::latch start(kThieves + 1);

    std::vector<TakerLog> thiefLogs(kThieves);
    std::vector<std::thread> thieves;
    for (int i = 0; i < kThieves; ++i) {
      thieves.emplace_back([&, i] {
        start.arrive_and_wait();
        TakerLog& log = thiefLogs[static_cast<std::size_t>(i)];
        while (true) {
          const bool finished = ownerDone.load(std::memory_order_acquire);
          const StealResult<Record*> result = deque.steal();
          if (result.status == StealStatus::Taken) {
            log.record(result.item);
          } else if (result.status == StealStatus::Empty && finished) {
            break;
          }
        }
      });
    }

    TakerLog ownerLog;
    start.arrive_and_wait();
    for (std::int64_t id = 0; id < entries; ++id) {
      Record& record = records[static_cast<std::size_t>(id)];
      record.id = id;
      record.payload = PayloadFor(id);
      ownerStep(deque, &record, ownerLog);
    }
    while (const std::optional<Record*> left = deque.take()) {
      ownerLog.record(*left);
    }
    ownerDone.store(true, std::memory_order_release);
    for (std::thread& thief : thieves) {
      thief.join();
    }

    return check(entries, ownerLog, thiefLogs);
  }

  static StressDefects check(std::int64_t entries, const TakerLog& ownerLog,
                             const std::vector<TakerLog>& thiefLogs) {
    StressDefects defects;
    std::vector<int> takenCount(static_cast<std::size_t>(entries), 0);
    auto count = [&](const TakerLog& log) {
      defects.payloadMismatches += log.payloadMismatches;
      defects.nullEntries += log.nullEntries;
      for (std::int64_t id : log.ids) {
        ++takenCount[static_cast<std::size_t>(id)];
      }
    };
    count(ownerLog);
    for (const TakerLog& log : thiefLogs) {
      count(log);
      // Entries sit in the ring in push order and thieves take from the top, which only grows,
      // so each thief sees increasing ids.
      for (std::size_t i = 1; i < log.ids.size(); ++i) {
        if (log.ids[i] <= log.ids[i - 1]) {
          ++defects.stealsOutOfOrder;
        }
      }
    }
    for (std::int64_t id = 0; id < entries; ++id) {
      if (takenCount[static_cast<std::size_t>(id)] != 1) {
        ++defects.entriesNotTakenOnce;
        if (defects.firstEntriesNotTakenOnce.size() < 16) {
          defects.firstEntriesNotTakenOnce.push_back(id);
        }
      }
    }
    return defects;
  }
};

TYPED_TEST_SUITE(WorkStealingDequeStressTest, Forms, FormNames);

// A small ring under a mixed owner workload: pushes, takes, fills to capacity and wraps.
TYPED_TEST(WorkStealingDequeStressTest, MixedOwnerWorkloadTakesEveryEntryExactlyOnce) {
  XorShift64 rng(0x5EEDu);
  const StressDefects defects = this->run(
      200000, 32, [&](typename TestFixture::Deque& deque, Record* record, TakerLog& ownerLog) {
        while (!deque.push(record)) {
          if (const std::optional<Record*> taken = deque.take()) {
            ownerLog.record(*taken);
          }
        }
        if (rng.next() % 3 == 0) {
          if (const std::optional<Record*> taken = deque.take()) {
            ownerLog.record(*taken);
          }
        }
      });
  EXPECT_THAT(defects, Eq(StressDefects{}));
}

// The owner keeps at most one entry queued, so nearly every take races the thieves for the last
// entry.
TYPED_TEST(WorkStealingDequeStressTest, LastEntryRaceTakesEveryEntryExactlyOnce) {
  const StressDefects defects = this->run(
      200000, 4, [](typename TestFixture::Deque& deque, Record* record, TakerLog& ownerLog) {
        while (!deque.push(record)) {
          if (const std::optional<Record*> taken = deque.take()) {
            ownerLog.record(*taken);
          }
        }
        if (const std::optional<Record*> taken = deque.take()) {
          ownerLog.record(*taken);
        }
      });
  EXPECT_THAT(defects, Eq(StressDefects{}));
}

// The owner keeps two entries queued and takes both, so a take often finds one entry above a top
// that thieves are advancing, the case where only the store-load barriers keep the owner and a
// thief from taking the same entry without a contended claim.
TYPED_TEST(WorkStealingDequeStressTest, TwoEntryRaceTakesEveryEntryExactlyOnce) {
  std::int64_t pushedSinceTake = 0;
  const StressDefects defects = this->run(
      200000, 4, [&](typename TestFixture::Deque& deque, Record* record, TakerLog& ownerLog) {
        while (!deque.push(record)) {
          if (const std::optional<Record*> taken = deque.take()) {
            ownerLog.record(*taken);
          }
        }
        if (++pushedSinceTake < 2) {
          return;
        }
        pushedSinceTake = 0;
        for (int i = 0; i < 2; ++i) {
          if (const std::optional<Record*> taken = deque.take()) {
            ownerLog.record(*taken);
          }
        }
      });
  EXPECT_THAT(defects, Eq(StressDefects{}));
}

}  // namespace
}  // namespace donner::tasks
