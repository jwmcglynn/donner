#include "donner/base/tasks/Parker.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {
namespace {

using testing::Eq;

/// What a parker word holds, decoded for test diagnostics.
struct DecodedWord {
  std::uint64_t epoch = 0;
  bool waiting = false;

  friend bool operator==(const DecodedWord&, const DecodedWord&) = default;

  friend std::ostream& operator<<(std::ostream& os, const DecodedWord& word) {
    return os << "{epoch=" << word.epoch << " waiting=" << word.waiting << "}";
  }
};

template <typename Word>
DecodedWord Decode(Word word) {
  using UnsignedWord = std::make_unsigned_t<Word>;
  return DecodedWord{static_cast<UnsignedWord>(word) >> 1, (word & 1) != 0};
}

/// The word types every parker test runs at: the 32-bit and the 64-bit native wait types, so both
/// are compiled and tested on every platform whichever one \ref NativeParkerWord names.
using WordTypes = testing::Types<std::int32_t, std::int64_t>;

class WordTypeNames {
public:
  template <typename T>
  static std::string GetName(int /*index*/) {
    return std::is_same_v<T, std::int32_t> ? "Word32" : "Word64";
  }
};

/// Hooks that count the platform calls and pass them through.
template <typename WordT>
struct CountingHooks {
  using Word = WordT;
  static inline std::atomic<int> notifies{0};
  static inline std::atomic<int> waits{0};

  static void Wait(const std::atomic<Word>& word, Word token, std::memory_order order) {
    waits.fetch_add(1);
    word.wait(token, order);
  }
  static void NotifyOne(std::atomic<Word>& word) {
    notifies.fetch_add(1);
    word.notify_one();
  }
  static void BeforePrepareWait() {}
  static void AfterPrepareWait() {}
  static void BeforeWait() {}
  static void BetweenChangeAndNotify() {}

  static void Reset() {
    notifies = 0;
    waits = 0;
  }
};

// Single-thread model tests: every operation and boundary of one parker.

template <typename WordT>
class ParkerModelTest : public testing::Test {
protected:
  using Word = WordT;
  using UnsignedWord = std::make_unsigned_t<Word>;
  using Hooks = CountingHooks<Word>;
  using ParkerT = Parker<Hooks>;

  static constexpr int kBits = static_cast<int>(sizeof(Word) * 8);

  /// The word whose unsigned bits are \p bits.
  static Word FromBits(UnsignedWord bits) { return static_cast<Word>(bits); }

  void SetUp() override { Hooks::Reset(); }
};

TYPED_TEST_SUITE(ParkerModelTest, WordTypes, WordTypeNames);

TYPED_TEST(ParkerModelTest, NewParkerHasEpochZeroAndNoWaitingBit) {
  typename TestFixture::ParkerT parker;
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{0, false}));
}

TYPED_TEST(ParkerModelTest, UnparkWithNoWaiterAdvancesTheEpochAndNeverNotifies) {
  typename TestFixture::ParkerT parker;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_FALSE(parker.unpark()) << "unpark " << i << " found the waiting bit set";
  }
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{1000, false}));
  EXPECT_THAT(TestFixture::Hooks::notifies.load(), Eq(0))
      << "a change with no waiting bit must not take the notify path";
}

TYPED_TEST(ParkerModelTest, PrepareWaitReturnsTheOldWordWithTheWaitingBit) {
  typename TestFixture::ParkerT parker;
  parker.unpark();
  parker.unpark();

  const typename TestFixture::Word token = parker.prepareWait();
  EXPECT_THAT(Decode(token), Eq(DecodedWord{2, true}));
  EXPECT_THAT(parker.word(), Eq(token));

  parker.finishWait();
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{2, false}));
}

TYPED_TEST(ParkerModelTest, UnparkWithTheWaitingBitSetNotifiesOnceAndKeepsTheBit) {
  typename TestFixture::ParkerT parker;
  const typename TestFixture::Word token = parker.prepareWait();

  EXPECT_TRUE(parker.unpark());
  EXPECT_THAT(TestFixture::Hooks::notifies.load(), Eq(1));
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{1, true}));

  // The change landed after prepareWait, so the wait returns at once.
  parker.wait(token);
  parker.finishWait();
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{1, false}));

  EXPECT_FALSE(parker.unpark()) << "the cleared bit must stop further notifies";
  EXPECT_THAT(TestFixture::Hooks::notifies.load(), Eq(1));
}

// The word is signed, so the epoch's top bit is the sign bit: crossing it makes the word negative,
// and the epoch read from it must still be the next unsigned value, with the waiting bit kept.
TYPED_TEST(ParkerModelTest, EpochCrossesTheSignBitWithoutTouchingTheWaitingBit) {
  using UnsignedWord = typename TestFixture::UnsignedWord;
  const UnsignedWord signBit = UnsignedWord{1} << (TestFixture::kBits - 1);
  const std::uint64_t epochAtSignBit = std::uint64_t{1} << (TestFixture::kBits - 2);

  typename TestFixture::ParkerT clear(TestFixture::FromBits(signBit - 2));
  EXPECT_THAT(Decode(clear.word()), Eq(DecodedWord{epochAtSignBit - 1, false}));
  EXPECT_FALSE(clear.unpark());
  EXPECT_THAT(clear.word(), Eq(TestFixture::FromBits(signBit)));
  EXPECT_THAT(clear.word(), testing::Lt(0)) << "the change must carry into the sign bit";
  EXPECT_THAT(Decode(clear.word()), Eq(DecodedWord{epochAtSignBit, false}));
  EXPECT_THAT(TestFixture::ParkerT::EpochOf(clear.word()), Eq(epochAtSignBit));

  typename TestFixture::ParkerT waiting(TestFixture::FromBits(signBit - 2));
  const typename TestFixture::Word token = waiting.prepareWait();
  EXPECT_TRUE(waiting.unpark());
  EXPECT_THAT(TestFixture::Hooks::notifies.load(), Eq(1));
  EXPECT_THAT(Decode(waiting.word()), Eq(DecodedWord{epochAtSignBit, true}));
  waiting.wait(token);
  waiting.finishWait();
  EXPECT_THAT(Decode(waiting.word()), Eq(DecodedWord{epochAtSignBit, false}));
}

// At the top epoch a change wraps the whole word back to epoch zero, keeping the waiting bit.
TYPED_TEST(ParkerModelTest, EpochWrapsWithoutTouchingTheWaitingBit) {
  using UnsignedWord = typename TestFixture::UnsignedWord;
  const UnsignedWord allOnes = ~UnsignedWord{0};
  const std::uint64_t topEpoch = allOnes >> 1;

  typename TestFixture::ParkerT clear(TestFixture::FromBits(allOnes - 1));
  EXPECT_THAT(Decode(clear.word()), Eq(DecodedWord{topEpoch, false}));
  EXPECT_FALSE(clear.unpark());
  EXPECT_THAT(Decode(clear.word()), Eq(DecodedWord{0, false}));

  typename TestFixture::ParkerT waiting(TestFixture::FromBits(allOnes - 1));
  const typename TestFixture::Word token = waiting.prepareWait();
  EXPECT_THAT(Decode(token), Eq(DecodedWord{topEpoch, true}));
  EXPECT_TRUE(waiting.unpark());
  EXPECT_THAT(Decode(waiting.word()), Eq(DecodedWord{0, true}));
  waiting.wait(token);
  waiting.finishWait();
  EXPECT_THAT(Decode(waiting.word()), Eq(DecodedWord{0, false}));
}

TYPED_TEST(ParkerModelTest, ParkUntilATrueConditionNeitherSetsTheBitNorWaits) {
  typename TestFixture::ParkerT parker;
  parker.parkUntil([] { return true; });
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{0, false}));
  EXPECT_THAT(TestFixture::Hooks::waits.load(), Eq(0));
}

TYPED_TEST(ParkerModelTest, ParkUntilAConditionTrueOnTheRecheckClearsTheBitWithoutWaiting) {
  typename TestFixture::ParkerT parker;
  int checks = 0;
  parker.parkUntil([&] { return ++checks > 1; });
  EXPECT_THAT(checks, Eq(2));
  EXPECT_THAT(Decode(parker.word()), Eq(DecodedWord{0, false}));
  EXPECT_THAT(TestFixture::Hooks::waits.load(), Eq(0));
}

template <typename WordT>
class ParkerDeathTest : public testing::Test {};

TYPED_TEST_SUITE(ParkerDeathTest, WordTypes, WordTypeNames);

TYPED_TEST(ParkerDeathTest, ASecondPrepareWaitWithoutFinishWaitFailsAReleaseAssertion) {
  EXPECT_DEATH(
      {
        Parker<PlatformParkerHooks<TypeParam>> parker;
        (void)parker.prepareWait();
        (void)parker.prepareWait();
      },
      "A parker has one sleeper");
}

// Deterministic interleavings of a waker with a sleeper. The platform wait is replaced by a model
// of a kernel wait queue: a waiter compares the word and blocks atomically with respect to a
// notify, and only a notify wakes a blocked waiter, as with a real futex. A change without a
// notify leaves a blocked waiter blocked, which a real `std::atomic::wait` cannot show.

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

/// What the model wait queue saw, for assertions.
struct FutexRecord {
  int blocks = 0;     //!< Times a waiter found the word equal to its token and blocked.
  int notifies = 0;   //!< Notify calls.
  bool hung = false;  //!< A blocked waiter was never notified within the hang guard.

  friend bool operator==(const FutexRecord&, const FutexRecord&) = default;

  friend std::ostream& operator<<(std::ostream& os, const FutexRecord& record) {
    return os << "{blocks=" << record.blocks << " notifies=" << record.notifies
              << " hung=" << record.hung << "}";
  }
};

/// A model of the kernel wait queue for one parker word.
class ModelWaitQueue {
public:
  template <typename Word>
  void wait(const std::atomic<Word>& word, Word token, std::memory_order order) {
    std::unique_lock lock(mutex_);
    while (word.load(order) == token) {
      ++record_.blocks;
      blocked_ = true;
      cv_.notify_all();
      const std::uint64_t serial = wakeSerial_;
      const bool woken = cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                                        [&] { return wakeSerial_ != serial; });
      blocked_ = false;
      if (!woken) {
        // Gives the thread back so the case can fail instead of hanging.
        record_.hung = true;
        return;
      }
    }
  }

  void notifyOne() {
    std::lock_guard lock(mutex_);
    ++record_.notifies;
    ++wakeSerial_;
    cv_.notify_all();
  }

  /// Waits until a waiter is blocked. @return False if none blocked within the hang guard.
  [[nodiscard]] bool waitUntilBlocked() {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [this] { return blocked_; });
  }

  /// Waits until a waiter is blocked for the \p count-th time. @return False if that did not
  /// happen within the hang guard.
  [[nodiscard]] bool waitUntilBlockedTimes(int count) {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [&] { return blocked_ && record_.blocks >= count; });
  }

  [[nodiscard]] FutexRecord record() {
    std::lock_guard lock(mutex_);
    return record_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool blocked_ = false;
  std::uint64_t wakeSerial_ = 0;
  FutexRecord record_;
};

/// Hooks that use the model wait queue and hold the sleeper at a chosen step.
template <typename WordT>
struct ModelHooks {
  using Word = WordT;
  enum class Hold { None, BeforePrepareWait, AfterPrepareWait, BeforeWait, BetweenChangeAndNotify };

  static inline ModelWaitQueue* queue = nullptr;
  static inline Gate* gate = nullptr;
  static inline std::atomic<Hold> hold{Hold::None};

  static void Wait(const std::atomic<Word>& word, Word token, std::memory_order order) {
    queue->wait(word, token, order);
  }
  static void NotifyOne(std::atomic<Word>& /*word*/) { queue->notifyOne(); }
  static void BeforePrepareWait() { HoldAt(Hold::BeforePrepareWait); }
  static void AfterPrepareWait() { HoldAt(Hold::AfterPrepareWait); }
  static void BeforeWait() { HoldAt(Hold::BeforeWait); }
  static void BetweenChangeAndNotify() { HoldAt(Hold::BetweenChangeAndNotify); }

private:
  static void HoldAt(Hold step) {
    Hold expected = step;
    if (hold.compare_exchange_strong(expected, Hold::None)) {
      gate->arriveAndWait();
    }
  }
};

/// Releases the gate and joins a case's threads when the case ends by any path.
class ReleaseAndJoin {
public:
  ReleaseAndJoin(Gate& gate, std::initializer_list<std::thread*> threads)
      : gate_(gate), threads_(threads) {}

  ReleaseAndJoin(const ReleaseAndJoin&) = delete;
  ReleaseAndJoin& operator=(const ReleaseAndJoin&) = delete;

  ~ReleaseAndJoin() {
    gate_.release();
    for (std::thread* thread : threads_) {
      if (thread->joinable()) {
        thread->join();
      }
    }
  }

private:
  Gate& gate_;
  std::vector<std::thread*> threads_;
};

template <typename WordT>
class ParkerInterleavingTest : public testing::Test {
protected:
  using Hooks = ModelHooks<WordT>;

  void SetUp() override {
    Hooks::queue = &queue_;
    Hooks::gate = &gate_;
  }

  void TearDown() override {
    Hooks::hold = Hooks::Hold::None;
    Hooks::queue = nullptr;
    Hooks::gate = nullptr;
  }

  /// The waker's side: make the condition true, then change the parker.
  bool wake() {
    ready_.store(true, std::memory_order_release);
    return parker_.unpark();
  }

  void startSleeper() {
    sleeper_ = std::thread(
        [this] { parker_.parkUntil([this] { return ready_.load(std::memory_order_acquire); }); });
  }

  ModelWaitQueue queue_;
  Gate gate_;
  Parker<Hooks> parker_;
  std::atomic<bool> ready_{false};
  std::thread sleeper_;
};

TYPED_TEST_SUITE(ParkerInterleavingTest, WordTypes, WordTypeNames);

// The waker finishes before the sleeper sets its waiting bit: no notify, and the sleeper's
// re-check after setting the bit sees the condition, so it never waits.
TYPED_TEST(ParkerInterleavingTest, AWakeBeforeTheWaitingBitIsSeenByTheRecheck) {
  ReleaseAndJoin joinOnExit(this->gate_, {&this->sleeper_});
  TestFixture::Hooks::hold = TestFixture::Hooks::Hold::BeforePrepareWait;
  this->startSleeper();
  ASSERT_TRUE(this->gate_.waitForArrival());

  EXPECT_FALSE(this->wake()) << "the waiting bit is not set yet, so the waker must not notify";

  this->gate_.release();
  this->sleeper_.join();
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{0, 0, false}))
      << "the re-check must see the condition, so the sleeper never blocks";
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{1, false}));
  EXPECT_FALSE(this->gate_.heldPastHangGuard());
}

// The waker runs between the sleeper's waiting bit and its re-check: the waker notifies, and the
// re-check sees the condition.
TYPED_TEST(ParkerInterleavingTest, AWakeBetweenTheWaitingBitAndTheRecheckNotifies) {
  ReleaseAndJoin joinOnExit(this->gate_, {&this->sleeper_});
  TestFixture::Hooks::hold = TestFixture::Hooks::Hold::AfterPrepareWait;
  this->startSleeper();
  ASSERT_TRUE(this->gate_.waitForArrival());

  EXPECT_TRUE(this->wake()) << "the waiting bit is set, so the waker must notify";

  this->gate_.release();
  this->sleeper_.join();
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{0, 1, false}));
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{1, false}));
  EXPECT_FALSE(this->gate_.heldPastHangGuard());
}

// The waker runs after the sleeper's re-check failed and before it waits: the waker notifies, and
// the wait returns at once because the word no longer equals the token.
TYPED_TEST(ParkerInterleavingTest, AWakeBetweenTheRecheckAndTheWaitEndsTheWaitAtOnce) {
  ReleaseAndJoin joinOnExit(this->gate_, {&this->sleeper_});
  TestFixture::Hooks::hold = TestFixture::Hooks::Hold::BeforeWait;
  this->startSleeper();
  ASSERT_TRUE(this->gate_.waitForArrival());

  EXPECT_TRUE(this->wake());

  this->gate_.release();
  this->sleeper_.join();
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{0, 1, false}))
      << "the changed word must end the wait without blocking";
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{1, false}));
  EXPECT_FALSE(this->gate_.heldPastHangGuard());
}

// The waker runs after the sleeper blocked: only the notify can wake it.
TYPED_TEST(ParkerInterleavingTest, AWakeAfterTheSleeperBlockedNotifiesIt) {
  ReleaseAndJoin joinOnExit(this->gate_, {&this->sleeper_});
  this->startSleeper();
  ASSERT_TRUE(this->queue_.waitUntilBlocked());

  EXPECT_TRUE(this->wake());

  this->sleeper_.join();
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{1, 1, false}))
      << "the blocked sleeper must be notified, not left asleep";
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{1, false}));
}

// The waker is held after its change and before its notify while the sleeper is blocked: the change
// alone must not wake the sleeper, and the notify that follows must.
TYPED_TEST(ParkerInterleavingTest, AWakeHeldBetweenItsChangeAndItsNotifyStillWakesTheSleeper) {
  bool notified = false;
  std::thread waker;
  ReleaseAndJoin joinOnExit(this->gate_, {&this->sleeper_, &waker});
  this->startSleeper();
  ASSERT_TRUE(this->queue_.waitUntilBlocked());

  TestFixture::Hooks::hold = TestFixture::Hooks::Hold::BetweenChangeAndNotify;
  waker = std::thread([&] { notified = this->wake(); });
  ASSERT_TRUE(this->gate_.waitForArrival());
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{1, true}))
      << "the change must land before the notify";
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{1, 0, false}))
      << "nothing may notify the sleeper before the change, and the change alone must not wake it";

  this->gate_.release();
  waker.join();
  this->sleeper_.join();
  EXPECT_TRUE(notified);
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{1, 1, false}));
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{1, false}));
  EXPECT_FALSE(this->gate_.heldPastHangGuard());
}

// A change while the condition is still false wakes the sleeper, which re-checks and blocks again.
TYPED_TEST(ParkerInterleavingTest, AChangeWithoutTheConditionIsASpuriousWake) {
  ReleaseAndJoin joinOnExit(this->gate_, {&this->sleeper_});
  this->startSleeper();
  ASSERT_TRUE(this->queue_.waitUntilBlocked());

  EXPECT_TRUE(this->parker_.unpark());
  // The sleeper wakes, sets the bit again, and blocks on the new epoch.
  ASSERT_TRUE(this->queue_.waitUntilBlockedTimes(2));

  EXPECT_TRUE(this->wake());
  this->sleeper_.join();
  EXPECT_THAT(this->queue_.record(), Eq(FutexRecord{2, 2, false}));
  EXPECT_THAT(Decode(this->parker_.word()), Eq(DecodedWord{2, false}));
}

// Multi-thread stress with the real platform wait. Each case runs with the hooks compiled out and
// again with delays injected into the sleeper's window between its waiting bit and its wait.

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

/// Real platform wait and notify, with a short random delay at each sleeper step: usually a yield,
/// sometimes a few microseconds, so wakers land inside the window far more often.
template <typename WordT>
struct DelayedHooks {
  using Word = WordT;
  static void Wait(const std::atomic<Word>& word, Word token, std::memory_order order) {
    word.wait(token, order);
  }
  static void NotifyOne(std::atomic<Word>& word) { word.notify_one(); }
  static void BeforePrepareWait() { Delay(); }
  static void AfterPrepareWait() { Delay(); }
  static void BeforeWait() { Delay(); }
  static void BetweenChangeAndNotify() { Delay(); }

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

template <typename WordT>
struct PlainHooksName {
  using Hooks = PlatformParkerHooks<WordT>;
  static constexpr int kRounds = 20000;
  static constexpr const char* kName = "NoHooks";
  static void Delay() {}
};
template <typename WordT>
struct DelayedHooksName {
  using Hooks = DelayedHooks<WordT>;
  static constexpr int kRounds = 4000;
  static constexpr const char* kName = "InjectedDelay";
  static void Delay() { DelayedHooks<WordT>::Delay(); }
};

using HookForms = testing::Types<PlainHooksName<std::int32_t>, DelayedHooksName<std::int32_t>,
                                 PlainHooksName<std::int64_t>, DelayedHooksName<std::int64_t>>;

class HookFormNames {
public:
  template <typename T>
  static std::string GetName(int /*index*/) {
    using Word = typename T::Hooks::Word;
    return std::string(T::kName) + (std::is_same_v<Word, std::int32_t> ? "Word32" : "Word64");
  }
};

/**
 * Ends a stalled stress case: if the case has not finished within the hang guard, it sets a flag
 * the threads check after each wake and changes every parker until they all stop.
 */
template <typename ParkerT>
class StallBreaker {
public:
  explicit StallBreaker(std::vector<ParkerT*> parkers) : parkers_(std::move(parkers)) {
    thread_ = std::thread([this] {
      std::unique_lock lock(mutex_);
      if (cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                         [this] { return finished_; })) {
        return;
      }
      stalled_.store(true, std::memory_order_seq_cst);
      while (!finished_) {
        lock.unlock();
        for (ParkerT* parker : parkers_) {
          parker->unpark();
        }
        lock.lock();
        cv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return finished_; });
      }
    });
  }

  StallBreaker(const StallBreaker&) = delete;
  StallBreaker& operator=(const StallBreaker&) = delete;

  ~StallBreaker() { finish(); }

  /// Stops the breaker. Call once every thread has finished.
  void finish() {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] bool stalled() const { return stalled_.load(std::memory_order_seq_cst); }

private:
  std::vector<ParkerT*> parkers_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool finished_ = false;
  std::atomic<bool> stalled_{false};
  std::thread thread_;
};

/// The defects a ping-pong run looks for, collected so a failure reports them all.
struct PingPongDefects {
  int payloadMismatches = 0;
  int replyMismatches = 0;
  int roundsCompleted = 0;
  bool stalled = false;

  friend bool operator==(const PingPongDefects&, const PingPongDefects&) = default;

  friend std::ostream& operator<<(std::ostream& os, const PingPongDefects& defects) {
    return os << "{payloadMismatches=" << defects.payloadMismatches
              << " replyMismatches=" << defects.replyMismatches
              << " roundsCompleted=" << defects.roundsCompleted << " stalled=" << defects.stalled
              << "}";
  }
};

std::uint64_t PayloadFor(int round) {
  return static_cast<std::uint64_t>(round) * 0x9E3779B97F4A7C15ull + 1;
}

template <typename NameT>
class ParkerStressTest : public testing::Test {
protected:
  using ParkerT = Parker<typename NameT::Hooks>;
  using Word = typename ParkerT::Word;
  using UnsignedWord = typename ParkerT::UnsignedWord;
  static constexpr int kRounds = NameT::kRounds;

  /**
   * Waits for the next change of \p parker after \p seenEpoch, using the parker word alone: either
   * `prepareWait` reads the change, or the wait's load does. Both acquire it, so the caller may
   * then read what the waker wrote with plain loads.
   */
  static void AwaitChange(ParkerT& parker, UnsignedWord seenEpoch) {
    NameT::Delay();
    const Word token = parker.prepareWait();
    NameT::Delay();
    if (ParkerT::EpochOf(token) == seenEpoch) {
      parker.wait(token);
    }
    parker.finishWait();
  }
};

TYPED_TEST_SUITE(ParkerStressTest, HookForms, HookFormNames);

// Two threads hand a plain payload back and forth, synchronized by the two parker words alone.
// The payload is not atomic, so ThreadSanitizer reports a race if a change does not publish it,
// and a lost wakeup stalls the case.
TYPED_TEST(ParkerStressTest, PingPongPublishesEveryPayloadThroughTheParkerWord) {
  using ParkerT = typename TestFixture::ParkerT;
  constexpr int kRounds = TestFixture::kRounds;
  ParkerT toResponder;
  ParkerT toInitiator;
  std::uint64_t request = 0;  // Written by the initiator, read by the responder.
  std::uint64_t reply = 0;    // Written by the responder, read by the initiator.
  PingPongDefects defects;
  int responderMismatches = 0;
  StallBreaker<ParkerT> breaker({&toResponder, &toInitiator});

  std::thread responder([&] {
    for (int round = 0; round < kRounds; ++round) {
      TestFixture::AwaitChange(toResponder, static_cast<typename TestFixture::UnsignedWord>(round));
      if (breaker.stalled()) {
        return;
      }
      if (request != PayloadFor(round)) {
        ++responderMismatches;
      }
      reply = request + 1;
      toInitiator.unpark();
    }
  });

  for (int round = 0; round < kRounds; ++round) {
    request = PayloadFor(round);
    toResponder.unpark();
    TestFixture::AwaitChange(toInitiator, static_cast<typename TestFixture::UnsignedWord>(round));
    if (breaker.stalled()) {
      break;
    }
    if (reply != PayloadFor(round) + 1) {
      ++defects.replyMismatches;
    }
    ++defects.roundsCompleted;
  }
  responder.join();
  breaker.finish();
  defects.payloadMismatches = responderMismatches;
  defects.stalled = breaker.stalled();

  EXPECT_THAT(defects, Eq(PingPongDefects{0, 0, kRounds, false}));
  EXPECT_THAT(Decode(toResponder.word()),
              Eq(DecodedWord{static_cast<std::uint64_t>(kRounds), false}));
  EXPECT_THAT(Decode(toInitiator.word()),
              Eq(DecodedWord{static_cast<std::uint64_t>(kRounds), false}));
}

// The scheduler's pattern: each side sleeps with parkUntil on a turn counter it reads with acquire
// ordering, and the other side advances the turn, then changes the parker.
TYPED_TEST(ParkerStressTest, ParkUntilPingPongNeverLosesAWake) {
  using ParkerT = typename TestFixture::ParkerT;
  constexpr int kRounds = TestFixture::kRounds;
  ParkerT parkers[2];
  std::atomic<int> turn{0};  // Even: thread 0 runs; odd: thread 1 runs.
  std::uint64_t box = 0;     // Plain; handed over with the turn.
  int mismatches[2] = {0, 0};
  StallBreaker<ParkerT> breaker({&parkers[0], &parkers[1]});

  auto play = [&](int side) {
    for (int step = side; step < 2 * kRounds; step += 2) {
      parkers[side].parkUntil(
          [&] { return turn.load(std::memory_order_acquire) == step || breaker.stalled(); });
      if (breaker.stalled()) {
        return;
      }
      if (box != static_cast<std::uint64_t>(step)) {
        ++mismatches[side];
      }
      box = static_cast<std::uint64_t>(step) + 1;
      turn.store(step + 1, std::memory_order_release);
      parkers[1 - side].unpark();
    }
  };
  std::thread other(play, 1);
  play(0);
  other.join();
  breaker.finish();

  EXPECT_FALSE(breaker.stalled()) << "a wake was lost at turn " << turn.load();
  EXPECT_THAT(turn.load(), Eq(2 * kRounds));
  EXPECT_THAT(mismatches[0], Eq(0));
  EXPECT_THAT(mismatches[1], Eq(0));
  EXPECT_THAT(Decode(parkers[0].word()).waiting, Eq(false));
  EXPECT_THAT(Decode(parkers[1].word()).waiting, Eq(false));
}

// Several wakers change one sleeper's parker concurrently while it sleeps and wakes. Every change
// is a read-modify-write, the sleeper's included, so none is lost: the final epoch counts them all.
TYPED_TEST(ParkerStressTest, ConcurrentWakersLoseNoChange) {
  using ParkerT = typename TestFixture::ParkerT;
  constexpr int kWakers = 4;
  const int perWaker = TestFixture::kRounds / 2;
  ParkerT parker;
  std::atomic<int> done{0};
  StallBreaker<ParkerT> breaker({&parker});

  std::vector<std::thread> wakers;
  for (int i = 0; i < kWakers; ++i) {
    wakers.emplace_back([&] {
      for (int j = 0; j < perWaker; ++j) {
        done.fetch_add(1, std::memory_order_release);
        parker.unpark();
      }
    });
  }
  parker.parkUntil([&] {
    return done.load(std::memory_order_acquire) == kWakers * perWaker || breaker.stalled();
  });
  for (std::thread& waker : wakers) {
    waker.join();
  }
  breaker.finish();

  ASSERT_FALSE(breaker.stalled()) << "the sleeper missed the last wake";
  EXPECT_THAT(Decode(parker.word()),
              Eq(DecodedWord{static_cast<std::uint64_t>(kWakers * perWaker), false}))
      << "every waker's change must survive the sleeper's own read-modify-writes";
}

}  // namespace
}  // namespace donner::tasks
