/// @file
/// Publication, ownership and fair service of adopted deques.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "donner/base/tasks/AsyncTaskScope.h"
#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/CoroutineTestUtils.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::Eq;
using testing::Le;

/// An optional recruitment hint may be missed; these tests require the mandatory publication.
class PublishHooks : public StepHooks {
public:
  [[nodiscard]] bool allowParallelismWake() const override { return false; }
};

TEST(AdoptedPublishTest, PushAfterTheSleepRecheckWakesAWorkerBeforeTheOwnerWaits) {
  Gate beforeWait;
  Event ran;
  int worker = -1;
  PublishHooks hooks;
  hooks.holdAt(SchedulerStep::BeforeSleeperWaits, 1, &beforeWait);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  const bool parked = beforeWait.waitForArrival();
  auto slot = scheduler->adopt();
  TaskScope work(*scheduler);
  const bool queued = work.trySpawn([&] {
    worker = Peer::WorkerIndex(*scheduler);
    ran.set();
  });
  const bool woken = Peer::State(*scheduler, 0) == WorkerState::Awake;
  EXPECT_THAT(Peer::GlobalQueued(*scheduler), Eq(0));
  beforeWait.release();
  const bool finishedByWorker = queued && woken && ran.wait();
  work.wait();

  EXPECT_TRUE(parked);
  EXPECT_TRUE(static_cast<bool>(slot));
  EXPECT_TRUE(queued);
  EXPECT_TRUE(woken);
  EXPECT_TRUE(finishedByWorker);
  EXPECT_THAT(worker, Eq(0));
  EXPECT_FALSE(beforeWait.heldPastHangGuard());
}

TEST(AdoptedPublishTest, SleepRecheckFindsTheAdoptedPushWhileItsPublisherIsHeld) {
  Gate prepared;
  Gate published;
  PublishHooks hooks;
  hooks.holdAt(SchedulerStep::AfterWaitingBit, 1, &prepared);
  hooks.holdAt(SchedulerStep::AfterAdoptedPush, 1, &published);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  const bool atWaitingBit = prepared.waitForArrival();
  std::atomic<bool> ranOnWorker{false};
  std::thread publisher([&] {
    auto slot = scheduler->adopt();
    EXPECT_TRUE(static_cast<bool>(slot));
    TaskScope scope(*scheduler);
    scope.spawn([&] { ranOnWorker.store(Peer::WorkerIndex(*scheduler) == 0); });
  });

  const bool pushed = published.waitForArrival();
  EXPECT_THAT(Peer::GlobalQueued(*scheduler), Eq(0));
  prepared.release();
  const bool decided =
      pushed && PollUntil([&] {
        return ranOnWorker.load() || hooks.count(SchedulerStep::BeforeSleeperWaits) != 0;
      });
  const bool seenWithoutPublishWake = ranOnWorker.load();
  published.release();
  publisher.join();

  EXPECT_TRUE(atWaitingBit);
  EXPECT_TRUE(pushed);
  EXPECT_TRUE(decided);
  EXPECT_TRUE(seenWithoutPublishWake);
  EXPECT_FALSE(prepared.heldPastHangGuard());
  EXPECT_FALSE(published.heldPastHangGuard());
}

CoroutineTestRoot SpawnAndJoinAsync(TaskScheduler& scheduler, std::atomic<int>& ranOn,
                                    std::atomic<int>& resumedOn) {
  AsyncTaskScope scope(scheduler);
  scope.spawn([&] { ranOn = Peer::WorkerIndex(scheduler); });
  co_await scope.join();
  resumedOn = Peer::WorkerIndex(scheduler);
}

TEST(AdoptedSchedulingTest, ReleasingTheSlotPublishesAnAsyncRecordAndPreservesItsPoolCharge) {
  Gate parked;
  Gate draining;
  StepHooks hooks;
  hooks.holdAt(SchedulerStep::BeforeSleeperWaits, 1, &parked);
  hooks.holdAt(SchedulerStep::AfterPublishPush, 1, &draining);
  Event done;
  std::atomic<int> ranOn{-1};
  std::atomic<int> resumedOn{-1};
  auto scheduler = Peer::Make(
      TaskSchedulerOptions{.workerCount = 1, .maxTasksPerCreator = 1, .adoptableThreads = 1},
      &hooks);
  auto coroutine = SpawnAndJoinAsync(*scheduler, ranOn, resumedOn);
  const bool workerHeld = parked.waitForArrival();
  std::thread firstOwner([&] {
    auto first = scheduler->adopt();
    EXPECT_TRUE(static_cast<bool>(first));
    coroutine.start(done);
    EXPECT_THAT(Peer::GlobalQueued(*scheduler), Eq(0));
  });
  const bool publishedDuringRelease = draining.waitForArrival();
  {
    // The worker cannot take the drained entry, and the releasing owner has not finished
    // publishing it. The only slot must remain unavailable throughout this interval.
    auto premature = scheduler->adopt();
    EXPECT_FALSE(static_cast<bool>(premature));
  }
  draining.release();
  firstOwner.join();
  EXPECT_THAT(Peer::GlobalQueued(*scheduler), Eq(1));
  bool joined = false;
  {
    auto replacement = scheduler->adopt();
    EXPECT_TRUE(static_cast<bool>(replacement));
    TaskScope work(*scheduler);
    EXPECT_FALSE(work.trySpawn([] {})) << "the drained record still occupies the slot's pool";
    parked.release();
    joined = done.wait();
    EXPECT_TRUE(work.trySpawn([] {})) << "the finisher returned the drained record";
  }
  scheduler.reset();
  EXPECT_TRUE(workerHeld);
  EXPECT_TRUE(publishedDuringRelease);
  EXPECT_FALSE(draining.heldPastHangGuard());
  EXPECT_FALSE(parked.heldPastHangGuard());
  EXPECT_TRUE(joined);
  EXPECT_THAT(ranOn.load(), Eq(0));
  EXPECT_THAT(resumedOn.load(), Eq(0));
}

TEST(AdoptedSchedulingTest, InnerWaitLeavesNewAsyncWorkForAWorker) {
  Gate parked;
  StepHooks hooks;
  hooks.holdAt(SchedulerStep::BeforeSleeperWaits, 1, &parked);
  Event asyncDone;
  constexpr int kNotRun = -2;  // The adopted caller has worker index -1.
  std::atomic<int> asyncWorker{kNotRun};
  std::atomic<int> resumedOn{-1};
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  auto coroutine = SpawnAndJoinAsync(*scheduler, asyncWorker, resumedOn);
  const bool workerHeld = parked.waitForArrival();
  const auto owner = std::this_thread::get_id();
  std::thread::id syncThread;
  {
    auto slot = scheduler->adopt();
    EXPECT_TRUE(static_cast<bool>(slot));
    TaskScope inner(*scheduler);
    coroutine.start(asyncDone);
    inner.spawn([&] { syncThread = std::this_thread::get_id(); });
    inner.wait();
    EXPECT_THAT(syncThread, Eq(owner));
    EXPECT_THAT(Peer::GlobalQueued(*scheduler), Eq(1));
    EXPECT_THAT(asyncWorker.load(), Eq(kNotRun));
  }
  parked.release();
  const bool joined = asyncDone.wait();
  scheduler.reset();
  EXPECT_TRUE(workerHeld);
  EXPECT_FALSE(parked.heldPastHangGuard());
  EXPECT_TRUE(joined);
  EXPECT_THAT(asyncWorker.load(), Eq(0));
  EXPECT_THAT(resumedOn.load(), Eq(0));
}

CoroutineTestRoot JoinExistingAsync(AsyncTaskScope& scope) {
  co_await scope.join();
}

TEST(AdoptedSchedulingDeathTest, OlderAsyncScopeCannotSubmitInsideASynchronousScope) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const auto misuse = [](bool tryOnly) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
    auto slot = scheduler.adopt();
    AsyncTaskScope older(scheduler);
    {
      TaskScope inner(scheduler);
      if (tryOnly) {
        (void)older.trySpawn([] {});
      } else {
        older.spawn([] {});
      }
    }
    auto joiner = JoinExistingAsync(older);
    Event done;
    joiner.start(done);
    EXPECT_TRUE(done.wait());
  };
  EXPECT_DEATH(misuse(false), "innermost");
  EXPECT_DEATH(misuse(true), "innermost");
}

TEST(AdoptedSchedulingDeathTest, OlderAsyncOnAnotherSchedulerIsRejectedBeforeInlineFallback) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const auto misuse = [](bool tryOnly) {
    TaskScheduler owner(TaskSchedulerOptions{.workerCount = 1});
    TaskScheduler inlineTarget(TaskSchedulerOptions{.workerCount = 0});
    auto slot = owner.adopt();
    AsyncTaskScope older(inlineTarget);
    {
      TaskScope inner(owner);
      if (tryOnly) {
        (void)older.trySpawn([] {});
      } else {
        older.spawn([] {});
      }
    }
    auto joiner = JoinExistingAsync(older);
    Event done;
    joiner.start(done);
    EXPECT_TRUE(done.wait());
  };
  EXPECT_DEATH(misuse(false), "innermost");
  EXPECT_DEATH(misuse(true), "innermost");
}

CoroutineTestRoot TryYoungerAsync(TaskScheduler& target, bool& queued, std::atomic<int>& ranOn) {
  AsyncTaskScope younger(target);
  const auto record = [&] { ranOn.store(Peer::WorkerIndex(target)); };
  queued = younger.trySpawn(record);
  if (!queued) {
    younger.spawn(record);
  }
  co_await younger.join();
}

TEST(AdoptedSchedulingTest, YoungerAsyncOnAnotherSchedulerKeepsTheTargetsAdmissionRules) {
  TaskScheduler owner(TaskSchedulerOptions{.workerCount = 1});
  for (std::size_t workers : {0u, 1u}) {
    Event done;
    std::atomic<int> ranOn{-2};
    bool queued = false;
    auto target = std::make_unique<TaskScheduler>(TaskSchedulerOptions{.workerCount = workers});
    auto slot = owner.adopt();
    EXPECT_TRUE(static_cast<bool>(slot));
    TaskScope inner(owner);
    auto joiner = TryYoungerAsync(*target, queued, ranOn);
    joiner.start(done);
    const bool completed = done.wait();
    target.reset();
    EXPECT_TRUE(completed);
    EXPECT_THAT(queued, Eq(workers != 0));
    EXPECT_THAT(ranOn.load(), Eq(workers == 0 ? -1 : 0));
  }
}

TEST(AdoptedSchedulingDeathTest, ReusedSlotDoesNotMakeAPreviousOwnersAsyncScopeYounger) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        std::unique_ptr<AsyncTaskScope> previous;
        {
          auto oldOwner = scheduler.adopt();
          // Make the birth ordinal larger than the first scope of a fresh thread.
          for (int i = 0; i < 4; ++i) {
            TaskScope earlier(scheduler);
          }
          previous = std::make_unique<AsyncTaskScope>(scheduler);
        }
        std::thread nextOwner([&] {
          auto sameSlot = scheduler.adopt();
          TaskScope inner(scheduler);
          previous->spawn([] {});
        });
        nextOwner.join();
        auto joiner = JoinExistingAsync(*previous);
        Event done;
        joiner.start(done);
        EXPECT_TRUE(done.wait());
      },
      "innermost");
}

TEST(AdoptedSchedulingTest, PeriodicAndIdleSearchesServeAllThreeOutsideSourcesCyclically) {
  constexpr unsigned kSources = 3;
  constexpr unsigned kPerSource = 4;
  constexpr unsigned kCheckInterval = 61;
  constexpr unsigned kLocals = 2 * kSources * kCheckInterval;
  struct Take {
    unsigned source;
    unsigned sequence;
    unsigned locals;
    int worker;
  };
  Gate workerHeld;
  std::array<Gate, 2> publisherHeld;
  std::atomic<unsigned> locals{0};
  Completion outside;
  std::mutex mutex;
  std::vector<Take> order;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .adoptableThreads = 2});
  const auto record = [&](unsigned source, unsigned sequence) {
    {
      std::lock_guard lock(mutex);
      order.push_back({source, sequence, locals.load(), Peer::WorkerIndex(scheduler)});
    }
    outside.arrive();
  };
  TaskScope global(scheduler);
  global.spawn([&] {
    for (unsigned i = 0; i < kLocals; ++i) {
      global.spawn([&] { ++locals; });
    }
    workerHeld.arriveAndWait();
  });
  const bool busy = workerHeld.waitForArrival();
  const auto publish = [&](unsigned slot) {
    auto adopted = scheduler.adopt();
    EXPECT_TRUE(static_cast<bool>(adopted));
    TaskScope batch(scheduler);
    for (unsigned i = 0; i < kPerSource; ++i) {
      batch.spawn([&, slot, i] { record(slot + 1, i); });
    }
    publisherHeld[slot].arriveAndWait();
  };
  std::thread first(publish, 0);
  const bool firstReady = publisherHeld[0].waitForArrival();
  std::thread second(publish, 1);
  const bool secondReady = publisherHeld[1].waitForArrival();
  for (unsigned i = 0; i < kPerSource; ++i) {
    global.spawn([&, i] { record(0, i); });
  }
  EXPECT_THAT(Peer::GlobalQueued(scheduler), Eq(kPerSource));
  workerHeld.release();
  const bool allServed = outside.waitFor(kPerSource * kSources);
  for (Gate& gate : publisherHeld) {
    gate.release();
  }
  first.join();
  second.join();
  global.wait();

  EXPECT_TRUE(busy && firstReady && secondReady && allServed);
  EXPECT_FALSE(workerHeld.heldPastHangGuard());
  for (Gate& gate : publisherHeld) {
    EXPECT_FALSE(gate.heldPastHangGuard());
  }
  EXPECT_THAT(locals.load(), Eq(kLocals));
  ASSERT_THAT(order.size(), Eq(kPerSource * kSources));
  std::array<unsigned, kSources> served{};
  std::array<unsigned, kSources> previous{};
  std::array<unsigned, kSources> previousLocals{};
  for (unsigned i = 0; i < order.size(); ++i) {
    const Take& take = order[i];
    EXPECT_THAT(take.worker, Eq(0));
    EXPECT_THAT(take.sequence, Eq(served[take.source]));
    ++served[take.source];
    EXPECT_THAT(i + 1 - previous[take.source], Le(kSources));
    EXPECT_THAT(take.locals - previousLocals[take.source], Le(kCheckInterval * kSources));
    previous[take.source] = i + 1;
    previousLocals[take.source] = take.locals;
  }
  for (unsigned count : served) {
    EXPECT_THAT(count, Eq(kPerSource));
  }
}

TEST(AdoptedSchedulingTest, LosingAnAdoptedClaimRetriesThatSourceBeforeTheGlobalQueue) {
  struct ClaimHooks : SchedulerTestHooks {
    void at(SchedulerStep step, std::size_t worker) override {
      if (step == SchedulerStep::BeforeAdoptedStealClaim && !held[worker]) {
        held[worker] = true;
        beforeClaim[worker].arriveAndWait();
      }
    }
    std::array<bool, 2> held{};  //!< Each element belongs to the worker with that index.
    std::array<Gate, 2> beforeClaim;
  } hooks;
  std::array<Gate, 2> globalRoots;
  Gate publisherReady;
  Gate oldestRunning;
  Completion tail;
  std::atomic<unsigned> sequence{0};
  unsigned secondAdoptedOrder = 0;
  unsigned globalOrder = 0;
  int oldestWorker = -1;
  int secondWorker = -1;
  auto scheduler =
      Peer::Make(TaskSchedulerOptions{.workerCount = 2,
                                      .idleSpinRounds = std::numeric_limits<std::size_t>::max(),
                                      .adoptableThreads = 1},
                 &hooks);
  TaskScope global(*scheduler);
  for (Gate& gate : globalRoots) {
    global.spawn([&gate] { gate.arriveAndWait(); });
  }
  const bool rootsHeld = globalRoots[0].waitForArrival() && globalRoots[1].waitForArrival();
  std::thread publisher([&] {
    auto adopted = scheduler->adopt();
    EXPECT_TRUE(static_cast<bool>(adopted));
    TaskScope batch(*scheduler);
    batch.spawn([&] {
      oldestWorker = Peer::WorkerIndex(*scheduler);
      oldestRunning.arriveAndWait();
    });
    batch.spawn([&] {
      secondWorker = Peer::WorkerIndex(*scheduler);
      secondAdoptedOrder = ++sequence;
      tail.arrive();
    });
    publisherReady.arriveAndWait();
  });
  const bool posted = publisherReady.waitForArrival();
  global.spawn([&] {
    globalOrder = ++sequence;
    tail.arrive();
  });
  for (Gate& gate : globalRoots) {
    gate.release();
  }
  // Each worker just served a global root: its cursor points to the adopted slot. Both read
  // the oldest adopted record before either is allowed to change the deque's top.
  const bool bothClaims =
      hooks.beforeClaim[0].waitForArrival() && hooks.beforeClaim[1].waitForArrival();
  hooks.beforeClaim[0].release();
  const bool oldestClaimed = bothClaims && oldestRunning.waitForArrival();
  hooks.beforeClaim[1].release();
  const bool tailFinished = oldestClaimed && tail.waitFor(2);
  oldestRunning.release();
  publisherReady.release();
  publisher.join();
  global.wait();

  EXPECT_TRUE(rootsHeld && posted && bothClaims && oldestClaimed && tailFinished);
  EXPECT_FALSE(oldestRunning.heldPastHangGuard());
  EXPECT_FALSE(publisherReady.heldPastHangGuard());
  for (std::size_t i = 0; i < globalRoots.size(); ++i) {
    EXPECT_FALSE(globalRoots[i].heldPastHangGuard());
    EXPECT_FALSE(hooks.beforeClaim[i].heldPastHangGuard());
  }
  EXPECT_THAT(oldestWorker, Eq(0));
  EXPECT_THAT(secondWorker, Eq(1));
  EXPECT_THAT(secondAdoptedOrder, Eq(1u));
  EXPECT_THAT(globalOrder, Eq(2u));
}

}  // namespace
}  // namespace donner::tasks
