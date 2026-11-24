#include "donner/base/tasks/TaskScheduler.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {

/// Reaches the scheduler's private spawn entry points and counters. Tasks here have no scope:
/// tests observe completion through their own latches and counters.
class TaskSchedulerTestPeer {
public:
  /// Constructs a scheduler with a test seam.
  static std::unique_ptr<TaskScheduler> Make(TaskSchedulerOptions options,
                                             SchedulerTestHooks* hooks) {
    return std::unique_ptr<TaskScheduler>(new TaskScheduler(options, hooks));
  }

  template <typename F>
  static bool TrySpawn(TaskScheduler& scheduler, F& work) {
    return scheduler.trySpawn(work);
  }

  template <typename F>
  static void Spawn(TaskScheduler& scheduler, F&& work) {
    scheduler.spawn(std::forward<F>(work));
  }

  static std::int64_t AwakeCount(const TaskScheduler& scheduler) {
    return scheduler.awake_.load(std::memory_order_seq_cst);
  }

  static std::int64_t AwakeStates(const TaskScheduler& scheduler) {
    std::int64_t count = 0;
    for (const auto& worker : scheduler.workers_) {
      if (worker->state.load(std::memory_order_seq_cst) == WorkerState::Awake) {
        ++count;
      }
    }
    return count;
  }

  static WorkerState State(const TaskScheduler& scheduler, std::size_t worker) {
    return scheduler.workers_[worker]->state.load(std::memory_order_seq_cst);
  }

  static const Parker<>* WorkerParker(const TaskScheduler& scheduler, std::size_t worker) {
    return &scheduler.workers_[worker]->parker;
  }

  static std::int64_t GlobalQueued(const TaskScheduler& scheduler) {
    return scheduler.globalQueued_.load(std::memory_order_seq_cst);
  }

  /// The calling thread's worker index in \p scheduler, or -1 if it is not one of its workers.
  static int WorkerIndex(const TaskScheduler& scheduler) {
    const TaskScheduler::Worker* worker = scheduler.currentWorker();
    return worker == nullptr ? -1 : static_cast<int>(worker->index);
  }

  static std::size_t StealVictim(std::size_t self, std::size_t workers, std::size_t offset,
                                 std::size_t k) {
    return TaskScheduler::StealVictim(self, workers, offset, k);
  }
};

namespace {

using Peer = TaskSchedulerTestPeer;
using testing::Each;
using testing::ElementsAreArray;
using testing::Eq;
using testing::Ge;
using testing::Le;
using testing::Ne;

/// A one-shot event a thread waits for under the hang guard.
class Event {
public:
  void set() {
    std::lock_guard lock(mutex_);
    set_ = true;
    cv_.notify_all();
  }

  /// @return False if the event was not set within the hang guard.
  [[nodiscard]] bool wait() {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [this] { return set_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool set_ = false;
};

/// Counts finished tasks; a test waits for the count it expects under the hang guard.
class Completion {
public:
  void arrive() {
    std::lock_guard lock(mutex_);
    ++count_;
    cv_.notify_all();
  }

  /// @return False if \p count arrivals did not happen within the hang guard.
  [[nodiscard]] bool waitFor(int count) {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [&] { return count_ >= count; });
  }

  [[nodiscard]] int count() {
    std::lock_guard lock(mutex_);
    return count_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int count_ = 0;
};

/// A rendezvous that holds one thread until the test releases it.
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

/// Hooks that hold the thread reaching a chosen step for the \p occurrence-th time at a gate.
class HoldOnceHooks : public SchedulerTestHooks {
public:
  HoldOnceHooks(SchedulerStep step, Gate* gate, int occurrence = 1)
      : step_(step), gate_(gate), occurrence_(occurrence) {}

  void at(SchedulerStep step, std::size_t /*worker*/) override {
    if (step == step_ && reached_.fetch_add(1) + 1 == occurrence_) {
      gate_->arriveAndWait();
    }
  }

private:
  const SchedulerStep step_;
  Gate* const gate_;
  const int occurrence_;
  std::atomic<int> reached_{0};
};

/// Hooks that combine two holds.
class HoldTwiceHooks : public SchedulerTestHooks {
public:
  HoldTwiceHooks(HoldOnceHooks* first, HoldOnceHooks* second) : first_(first), second_(second) {}

  void at(SchedulerStep step, std::size_t worker) override {
    first_->at(step, worker);
    second_->at(step, worker);
  }

private:
  HoldOnceHooks* const first_;
  HoldOnceHooks* const second_;
};

/**
 * Reads a worker's parker epoch. The pointer is taken while the scheduler is alive; it stays valid
 * while that worker is held at a hook, since the destructor joins the worker before freeing it.
 */
class ParkerEpochWatch {
public:
  ParkerEpochWatch(const TaskScheduler& scheduler, std::size_t worker)
      : parker_(Peer::WorkerParker(scheduler, worker)) {}

  [[nodiscard]] std::uint64_t load() const { return Parker<>::EpochOf(parker_->word()); }

private:
  const Parker<>* parker_;
};

/// Polls \p condition under the hang guard. @return False if it never held.
template <typename Condition>
bool PollUntil(Condition condition) {
  const tests::HangGuard guard;
  while (guard.active()) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(tests::kPollInterval);
  }
  return condition();
}

TEST(TaskSchedulerTest, ZeroWorkersRunEverySpawnInlineAndTrySpawnFails) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
  EXPECT_THAT(scheduler.workerCount(), Eq(0u));
  EXPECT_TRUE(scheduler.threadingDisabledByConfiguration());

  bool ran = false;
  auto work = [&] { ran = true; };
  EXPECT_FALSE(Peer::TrySpawn(scheduler, work)) << "with no workers trySpawn must decline";
  EXPECT_FALSE(ran) << "a declined trySpawn must leave the work to the caller";

  std::thread::id ranOn;
  Peer::Spawn(scheduler, [&] { ranOn = std::this_thread::get_id(); });
  EXPECT_THAT(ranOn, Eq(std::this_thread::get_id())) << "spawn runs inline on the caller";
}

TEST(TaskSchedulerTest, StealRoundsProbeEveryOtherWorkerExactlyOnce) {
  for (std::size_t workers = 2; workers <= 8; ++workers) {
    for (std::size_t self = 0; self < workers; ++self) {
      for (std::size_t offset = 0; offset + 1 < workers; ++offset) {
        std::vector<std::size_t> victims;
        for (std::size_t k = 0; k + 1 < workers; ++k) {
          victims.push_back(Peer::StealVictim(self, workers, offset, k));
        }
        std::set<std::size_t> expected;
        for (std::size_t i = 0; i < workers; ++i) {
          if (i != self) {
            expected.insert(i);
          }
        }
        EXPECT_THAT(std::set<std::size_t>(victims.begin(), victims.end()), Eq(expected))
            << "workers=" << workers << " self=" << self << " offset=" << offset;
        EXPECT_THAT(victims.size(), Eq(workers - 1));
      }
    }
  }
}

/// The worker counts every multi-worker case runs at.
class WorkerCounts : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(Workers, WorkerCounts, testing::Values(1, 4),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                           return "Workers" + std::to_string(info.param);
                         });

TEST_P(WorkerCounts, NonWorkerSpawnsRunExactlyOnceOnWorkers) {
  constexpr int kTasks = 2000;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  EXPECT_THAT(scheduler.workerCount(), Eq(GetParam()));
  EXPECT_FALSE(scheduler.threadingDisabledByConfiguration());

  std::vector<int> runs(kTasks, 0);
  std::vector<int> ranOnWorker(kTasks, -1);
  Completion done;
  for (int i = 0; i < kTasks; ++i) {
    Peer::Spawn(scheduler, [&, i] {
      ++runs[static_cast<std::size_t>(i)];
      ranOnWorker[static_cast<std::size_t>(i)] = Peer::WorkerIndex(scheduler);
      done.arrive();
    });
  }
  ASSERT_TRUE(done.waitFor(kTasks)) << "only " << done.count() << " of " << kTasks << " ran";
  EXPECT_THAT(runs, Each(Eq(1)));
  EXPECT_THAT(ranOnWorker, Each(Ge(0))) << "tasks from a non-worker run on workers";
}

TEST_P(WorkerCounts, TasksSpawnedOnWorkersRunExactlyOnce) {
  constexpr int kRoots = 64;
  constexpr int kChildren = 32;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  std::vector<std::atomic<int>> runs(kRoots * kChildren);
  Completion done;
  for (int root = 0; root < kRoots; ++root) {
    Peer::Spawn(scheduler, [&, root] {
      for (int child = 0; child < kChildren; ++child) {
        Peer::Spawn(scheduler, [&, root, child] {
          runs[static_cast<std::size_t>(root * kChildren + child)].fetch_add(1);
          done.arrive();
        });
      }
    });
  }
  ASSERT_TRUE(done.waitFor(kRoots * kChildren));
  for (const std::atomic<int>& count : runs) {
    EXPECT_THAT(count.load(), Eq(1));
  }
}

TEST(TaskSchedulerTest, TheGlobalQueueIsFirstInFirstOut) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  Gate busy;
  Completion done;
  std::vector<int> order;  // Written only by the one worker; read after its completions.
  Peer::Spawn(scheduler, [&] {
    busy.arriveAndWait();
    done.arrive();
  });
  ASSERT_TRUE(busy.waitForArrival());
  for (int i = 0; i < 20; ++i) {
    Peer::Spawn(scheduler, [&, i] {
      order.push_back(i);
      done.arrive();
    });
  }
  busy.release();
  ASSERT_TRUE(done.waitFor(21));
  std::vector<int> expected;
  for (int i = 0; i < 20; ++i) {
    expected.push_back(i);
  }
  EXPECT_THAT(order, ElementsAreArray(expected));
}

TEST(TaskSchedulerTest, TheExternalBoundDeclinesTrySpawnAndRunsSpawnInline) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .maxExternalTasks = 4});
  Gate busy;
  Completion done;
  Peer::Spawn(scheduler, [&] {
    busy.arriveAndWait();
    done.arrive();
  });
  ASSERT_TRUE(busy.waitForArrival());

  // The running task's record is live until it returns, so three more fill the bound.
  for (int i = 0; i < 3; ++i) {
    auto work = [&] { done.arrive(); };
    EXPECT_TRUE(Peer::TrySpawn(scheduler, work)) << "spawn " << i << " is within the bound";
  }
  bool declinedRan = false;
  auto declined = [&] { declinedRan = true; };
  EXPECT_FALSE(Peer::TrySpawn(scheduler, declined)) << "past the bound trySpawn must decline";
  EXPECT_FALSE(declinedRan);

  std::thread::id ranOn;
  Peer::Spawn(scheduler, [&] { ranOn = std::this_thread::get_id(); });
  EXPECT_THAT(ranOn, Eq(std::this_thread::get_id())) << "past the bound spawn runs inline";

  busy.release();
  ASSERT_TRUE(done.waitFor(4));
}

TEST(TaskSchedulerTest, AWorkerSpawnPastItsDequeOrItsRecordBoundIsDeclined) {
  struct Case {
    std::size_t dequeCapacity;
    std::size_t maxTasksPerCreator;
  };
  for (const Case limits : {Case{2, 64}, Case{64, 2}}) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1,
                                                 .dequeCapacity = limits.dequeCapacity,
                                                 .maxTasksPerCreator = limits.maxTasksPerCreator});
    Completion done;
    std::vector<bool> accepted;
    bool inlineRanBeforeReturn = false;
    Peer::Spawn(scheduler, [&] {
      for (int i = 0; i < 3; ++i) {
        auto child = [&] { done.arrive(); };
        accepted.push_back(Peer::TrySpawn(scheduler, child));
      }
      bool ran = false;
      Peer::Spawn(scheduler, [&] { ran = true; });
      inlineRanBeforeReturn = ran;
      done.arrive();
    });
    ASSERT_TRUE(done.waitFor(3)) << "deque " << limits.dequeCapacity;
    EXPECT_THAT(accepted, ElementsAreArray({true, true, false}))
        << "deque=" << limits.dequeCapacity << " records=" << limits.maxTasksPerCreator;
    EXPECT_TRUE(inlineRanBeforeReturn) << "a spawn the worker cannot queue runs inline";
  }
}

/// A closure that owns its state, so a closure moved out of it is visible: its value is gone, and
/// running what is left counts 100 instead of 1.
struct OwningClosure {
  std::unique_ptr<int> value;
  int* runs;

  void operator()() const { *runs += value != nullptr ? *value : 100; }
};

// A declined trySpawn must leave the closure with the caller, at every bound; spawn then runs it.
TEST(TaskSchedulerTest, ADeclinedTrySpawnLeavesTheClosureWithTheCaller) {
  // Zero workers.
  {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
    int runs = 0;
    OwningClosure closure{std::make_unique<int>(1), &runs};
    EXPECT_FALSE(Peer::TrySpawn(scheduler, closure));
    EXPECT_THAT(closure.value, testing::NotNull()) << "zero workers: the closure was taken";
    Peer::Spawn(scheduler, closure);
    EXPECT_THAT(runs, Eq(1));
  }
  // The external bound.
  {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .maxExternalTasks = 1});
    Gate busy;
    Completion done;
    Peer::Spawn(scheduler, [&] {
      busy.arriveAndWait();
      done.arrive();
    });
    ASSERT_TRUE(busy.waitForArrival());
    int runs = 0;
    OwningClosure closure{std::make_unique<int>(1), &runs};
    EXPECT_FALSE(Peer::TrySpawn(scheduler, closure));
    EXPECT_THAT(closure.value, testing::NotNull()) << "external bound: the closure was taken";
    Peer::Spawn(scheduler, closure);
    EXPECT_THAT(runs, Eq(1));
    busy.release();
    ASSERT_TRUE(done.waitFor(1));
  }
  // A worker's record bound, and a worker's full deque.
  struct Case {
    const char* name;
    std::size_t dequeCapacity;
    std::size_t maxTasksPerCreator;
  };
  for (const Case limits : {Case{"record bound", 64, 1}, Case{"full deque", 1, 64}}) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1,
                                                 .dequeCapacity = limits.dequeCapacity,
                                                 .maxTasksPerCreator = limits.maxTasksPerCreator});
    Completion done;
    bool declined = false;
    bool intactAfterDecline = false;
    int runs = 0;
    Peer::Spawn(scheduler, [&] {
      auto first = [&] { done.arrive(); };
      ASSERT_TRUE(Peer::TrySpawn(scheduler, first));
      OwningClosure closure{std::make_unique<int>(1), &runs};
      declined = !Peer::TrySpawn(scheduler, closure);
      intactAfterDecline = closure.value != nullptr;
      Peer::Spawn(scheduler, closure);
      done.arrive();
    });
    ASSERT_TRUE(done.waitFor(2)) << limits.name;
    EXPECT_TRUE(declined) << limits.name;
    EXPECT_TRUE(intactAfterDecline) << limits.name << ": the closure was taken";
    EXPECT_THAT(runs, Eq(1)) << limits.name;
  }
}

// A worker with endless local work still takes a global entry: one outside take every 61 tasks
// it runs from its own deque.
TEST(TaskSchedulerTest, AWorkerWithEndlessLocalWorkStillTakesTheGlobalQueue) {
  constexpr int kChain = 500;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  Gate busy;
  Completion done;
  int localRun = 0;           // Written only by the one worker.
  int localRunAtGlobal = -1;  // Written only by the one worker.

  std::function<void(int)> link = [&](int k) {
    ++localRun;
    if (k + 1 < kChain) {
      Peer::Spawn(scheduler, [&, k] { link(k + 1); });
    }
    done.arrive();
  };
  Peer::Spawn(scheduler, [&] {
    busy.arriveAndWait();
    Peer::Spawn(scheduler, [&] { link(0); });
  });
  ASSERT_TRUE(busy.waitForArrival());
  Peer::Spawn(scheduler, [&] {
    localRunAtGlobal = localRun;
    done.arrive();
  });
  busy.release();
  ASSERT_TRUE(done.waitFor(kChain + 1));
  EXPECT_THAT(localRunAtGlobal, Le(61))
      << "the global entry must not wait behind more than 61 local tasks";
}

// A busy worker's local entry is stolen by an idle worker that keeps making steal rounds.
TEST(TaskSchedulerTest, AnIdleWorkerStealsFromABusyOne) {
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = 2, .idleSpinRounds = std::size_t{1} << 40});
  Event childRan;
  int parentWorker = -1;
  int childWorker = -1;
  bool childSeen = false;
  Completion done;
  Peer::Spawn(scheduler, [&] {
    parentWorker = Peer::WorkerIndex(scheduler);
    Peer::Spawn(scheduler, [&] {
      childWorker = Peer::WorkerIndex(scheduler);
      childRan.set();
    });
    // Blocks inside the task: only a steal by the other worker can run the child.
    childSeen = childRan.wait();
    done.arrive();
  });
  ASSERT_TRUE(done.waitFor(1));
  EXPECT_TRUE(childSeen) << "the idle worker never stole the busy worker's child";
  EXPECT_THAT(childWorker, Ne(parentWorker));
}

// Deterministic interleavings of the publish pairing: a publisher's increment of the global count
// and its read of the awake count, against a sleeper's decrement and its re-check of the count.

// The publish lands before the sleeper decrements: the publisher sees the worker awake and does not
// wake it, and the sleeper's re-check sees the work.
TEST(TaskSchedulerInterleavingTest, APublishBeforeTheSleepersDecrementIsSeenByItsRecheck) {
  Gate sleeper;
  HoldOnceHooks hooks(SchedulerStep::AfterDrain, &sleeper);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  ASSERT_TRUE(sleeper.waitForArrival());

  Completion done;
  Peer::Spawn(*scheduler, [&] { done.arrive(); });
  EXPECT_THAT(Peer::AwakeCount(*scheduler), Eq(1)) << "the sleeper has not decremented yet";
  EXPECT_THAT(Peer::State(*scheduler, 0), Eq(WorkerState::Awake))
      << "the publisher saw the worker awake, so it must not have woken it";

  sleeper.release();
  ASSERT_TRUE(done.waitFor(1)) << "the sleeper's re-check must see the queued task";
  EXPECT_FALSE(sleeper.heldPastHangGuard());
}

// The sleeper decremented and found nothing; then a publish reads the awake count as zero and must
// wake it.
TEST(TaskSchedulerInterleavingTest, APublishAfterTheSleepersRecheckWakesIt) {
  Gate sleeper;
  HoldOnceHooks hooks(SchedulerStep::BeforeSleeperWaits, &sleeper);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  ASSERT_TRUE(sleeper.waitForArrival());
  ASSERT_THAT(Peer::AwakeCount(*scheduler), Eq(0));
  ASSERT_THAT(Peer::State(*scheduler, 0), Eq(WorkerState::AsleepShallow));

  Completion done;
  Peer::Spawn(*scheduler, [&] { done.arrive(); });
  EXPECT_THAT(Peer::State(*scheduler, 0), Eq(WorkerState::Awake))
      << "the publisher saw no worker awake, so it must swap one awake";
  EXPECT_THAT(Peer::AwakeCount(*scheduler), Eq(1));

  sleeper.release();
  ASSERT_TRUE(done.waitFor(1)) << "the publisher's parker change must end the sleeper's wait";
  EXPECT_FALSE(sleeper.heldPastHangGuard());
}

// A publisher wakes the worker while the worker, having seen the work, is about to wake itself:
// exactly one of the two swaps succeeds, so the worker is counted awake once.
TEST(TaskSchedulerInterleavingTest, ASelfWakeRacingAPublishersWakeCountsTheWorkerOnce) {
  Gate decremented;
  Gate nextSleep;
  HoldOnceHooks holdDecrement(SchedulerStep::AfterSleeperDecrement, &decremented);
  HoldOnceHooks holdNextSleep(SchedulerStep::AfterDrain, &nextSleep, /*occurrence=*/2);
  HoldTwiceHooks hooks(&holdDecrement, &holdNextSleep);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  ASSERT_TRUE(decremented.waitForArrival());

  Completion done;
  Peer::Spawn(*scheduler, [&] { done.arrive(); });
  ASSERT_THAT(Peer::State(*scheduler, 0), Eq(WorkerState::Awake));
  ASSERT_THAT(Peer::AwakeCount(*scheduler), Eq(1));

  decremented.release();
  ASSERT_TRUE(done.waitFor(1));
  ASSERT_TRUE(nextSleep.waitForArrival());
  EXPECT_THAT(Peer::AwakeCount(*scheduler), Eq(Peer::AwakeStates(*scheduler)))
      << "the failed self-wake swap must not count the worker a second time";
  EXPECT_THAT(Peer::AwakeCount(*scheduler), Eq(1));
  nextSleep.release();
}

// The shutdown pairing: the destructor's stop flag and its swap, against a sleeper's state store
// and its re-check of the flag.

/**
 * Everything a scheduler destroyed on another thread refers to, owned jointly by the test and that
 * thread, so a destroyer detached after a failed join touches nothing out of scope.
 */
struct ShutdownState {
  Gate gate;
  std::unique_ptr<HoldOnceHooks> hooks;
  std::unique_ptr<TaskScheduler> scheduler;
  Event destroyed;
};

/**
 * Destroys the state's scheduler on another thread, running \p whileDestroying meanwhile.
 *
 * @return True if the destructor returned within the hang guard. Otherwise the thread is detached
 *   and keeps the state alive.
 */
bool DestroyOnAnotherThread(const std::shared_ptr<ShutdownState>& state,
                            const std::function<void()>& whileDestroying) {
  std::thread destroyer([state] {
    state->scheduler.reset();
    state->destroyed.set();
  });
  whileDestroying();
  const bool joined = state->destroyed.wait();
  if (joined) {
    destroyer.join();
  } else {
    destroyer.detach();
  }
  return joined;
}

// The destructor runs while an idle worker is between draining and setting its waiting bit: its
// swap fails and its parker change lands before the waiting bit, so only the sleeper's re-check of
// the stop flag lets it exit.
TEST(TaskSchedulerInterleavingTest, ShutdownRacingAnIdleWorkersSleepJoins) {
  auto state = std::make_shared<ShutdownState>();
  state->hooks = std::make_unique<HoldOnceHooks>(SchedulerStep::AfterDrain, &state->gate);
  state->scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, state->hooks.get());
  ASSERT_TRUE(state->gate.waitForArrival());
  ParkerEpochWatch parkerEpoch(*state->scheduler, 0);

  bool changeLandedFirst = false;
  const bool joined = DestroyOnAnotherThread(state, [&] {
    // The destructor's swap fails, since the worker is still awake, and its parker change lands
    // before the sleeper sets its waiting bit.
    changeLandedFirst = PollUntil([&] { return parkerEpoch.load() == 1; });
    state->gate.release();
  });
  EXPECT_TRUE(changeLandedFirst) << "the destructor never changed the worker's parker";
  EXPECT_TRUE(joined) << "the destructor's join never returned";
}

// The destructor runs after the sleeper stored asleep: its swap succeeds and its parker change
// ends the wait.
TEST(TaskSchedulerInterleavingTest, ShutdownAfterTheSleeperStoredAsleepWakesIt) {
  auto state = std::make_shared<ShutdownState>();
  state->hooks = std::make_unique<HoldOnceHooks>(SchedulerStep::BeforeSleeperWaits, &state->gate);
  state->scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, state->hooks.get());
  ASSERT_TRUE(state->gate.waitForArrival());

  const bool joined = DestroyOnAnotherThread(state, [&] { state->gate.release(); });
  EXPECT_TRUE(joined) << "the destructor's join never returned";
}

// Stress: no queued task waits on a sleeping pool. Each case runs with no hooks and again with
// short random delays at every protocol step.

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

/// Hooks that delay the calling thread at every protocol step: usually a yield, sometimes a few
/// microseconds.
class DelayHooks : public SchedulerTestHooks {
public:
  void at(SchedulerStep /*step*/, std::size_t /*worker*/) override {
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

struct StressForm {
  std::size_t workers;
  bool delays;
  int rounds;
};

std::ostream& operator<<(std::ostream& os, const StressForm& form) {
  return os << form.workers << " workers" << (form.delays ? ", injected delays" : "");
}

class SchedulerStressTest : public testing::TestWithParam<StressForm> {
protected:
  /// The delay hooks have static storage, so a destroyer thread detached after a failed join
  /// never refers to a destroyed fixture.
  SchedulerTestHooks* hooks() {
    static DelayHooks delays;
    return GetParam().delays ? &delays : nullptr;
  }
};

INSTANTIATE_TEST_SUITE_P(Forms, SchedulerStressTest,
                         testing::Values(StressForm{1, false, 2000}, StressForm{4, false, 2000},
                                         StressForm{1, true, 500}, StressForm{4, true, 500}),
                         [](const testing::TestParamInfo<StressForm>& info) {
                           return "Workers" + std::to_string(info.param.workers) +
                                  (info.param.delays ? "InjectedDelay" : "NoHooks");
                         });

// One task at a time from a thread that is not a worker, each waited for: the workers go back to
// sleep between tasks, so every task is a publish racing a sleep. A lost wakeup leaves a task
// queued with every worker asleep, and its wait fails at the hang guard.
TEST_P(SchedulerStressTest, NoQueuedTaskWaitsOnASleepingPool) {
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = GetParam().workers}, hooks());
  for (int round = 0; round < GetParam().rounds; ++round) {
    Event ran;
    Peer::Spawn(*scheduler, [&] { ran.set(); });
    ASSERT_TRUE(ran.wait()) << "task " << round << " was never run";
  }
}

// Several threads that are not workers submit at once, and tasks spawn children on their workers.
TEST_P(SchedulerStressTest, ConcurrentSubmittersAndWorkerSpawnsRunEveryTaskOnce) {
  constexpr int kSubmitters = 3;
  const int perSubmitter = GetParam().rounds / 2;
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = GetParam().workers}, hooks());
  TaskScheduler& schedulerRef = *scheduler;
  std::vector<std::atomic<int>> runs(static_cast<std::size_t>(kSubmitters * perSubmitter * 2));
  Completion done;
  std::vector<std::thread> submitters;
  for (int s = 0; s < kSubmitters; ++s) {
    submitters.emplace_back([&, s] {
      for (int i = 0; i < perSubmitter; ++i) {
        const std::size_t id = static_cast<std::size_t>((s * perSubmitter + i) * 2);
        Peer::Spawn(schedulerRef, [&, id] {
          runs[id].fetch_add(1);
          Peer::Spawn(schedulerRef, [&, id] {
            runs[id + 1].fetch_add(1);
            done.arrive();
          });
          done.arrive();
        });
      }
    });
  }
  for (std::thread& submitter : submitters) {
    submitter.join();
  }
  ASSERT_TRUE(done.waitFor(static_cast<int>(runs.size())));
  for (const std::atomic<int>& count : runs) {
    EXPECT_THAT(count.load(), Eq(1));
  }
}

// Schedulers are destroyed while their idle workers go to sleep: the shutdown pairing races the
// sleep steps on every iteration. A destructor whose join never returns fails at the hang guard.
TEST_P(SchedulerStressTest, ShutdownRacingIdleWorkersAlwaysJoins) {
  const int iterations = GetParam().rounds / 10;
  for (int i = 0; i < iterations; ++i) {
    auto state = std::make_shared<ShutdownState>();
    state->scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = GetParam().workers}, hooks());
    if (i % 2 == 0) {
      Event ran;
      Peer::Spawn(*state->scheduler, [&] { ran.set(); });
      ASSERT_TRUE(ran.wait());
    }
    if (!DestroyOnAnotherThread(state, [] {})) {
      FAIL() << "iteration " << i << ": the destructor's join never returned";
    }
  }
}

}  // namespace
}  // namespace donner::tasks
