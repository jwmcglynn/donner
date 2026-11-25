/// @file
/// Search accounting and waking a parked thief after local publication.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>

#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/CoroutineTestUtils.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::Eq;
using testing::Ge;
using testing::Ne;

constexpr std::size_t kMaxWorkers = 4;

/// An injected hold, armed once for a chosen step and optionally a particular worker.
struct StepHold {
  void arm(SchedulerStep at, int worker = -1) {
    target.store(worker);
    step.store(static_cast<int>(at));
  }

  void visit(SchedulerStep at, std::size_t worker) {
    if (step.load() == static_cast<int>(at)) {
      const int selected = target.load();
      if (selected == -1 || selected == static_cast<int>(worker)) {
        gate.arriveAndWait();
      }
    }
  }

  Gate gate;
  std::atomic<int> step{-1};
  std::atomic<int> target{-1};
};

class SearchHooks : public SchedulerTestHooks {
public:
  SearchHooks(unsigned searchHolds, bool holdFirstPark)
      : searchHolds_(searchHolds), holdFirstPark_(holdFirstPark) {}

  void at(SchedulerStep step, std::size_t worker) override {
    hold.visit(step, worker);
    if (step == SchedulerStep::AfterSearchStart && (searchHolds_ & (1u << worker)) &&
        !heldSearch_[worker]) {
      heldSearch_[worker] = true;
      firstSearch[worker].arriveAndWait();
    } else if (step == SchedulerStep::BetweenWakeSwapAndParkerChange) {
      ++wakes[worker];
    } else if (step == SchedulerStep::BeforeSleeperWaits) {
      // A worker must have passed its visibility recheck since its most recent wake.
      parkEpoch_[worker].store(wakes[worker].load() + 1);
      if (worker == 0 && holdFirstPark_ && !heldPark_) {
        heldPark_ = true;
        firstPark.arriveAndWait();
      }
    }
  }

  bool parked(const TaskScheduler& scheduler, std::size_t worker,
              WorkerState state = WorkerState::AsleepShallow) const {
    return Peer::State(scheduler, worker) == state &&
           parkEpoch_[worker].load() == wakes[worker].load() + 1;
  }

  void release() {
    hold.gate.release();
    for (Gate& gate : firstSearch) {
      gate.release();
    }
    firstPark.release();
  }

  StepHold hold;
  std::array<Gate, kMaxWorkers> firstSearch;
  Gate firstPark;
  std::array<std::atomic<unsigned>, kMaxWorkers> wakes{};

private:
  const unsigned searchHolds_;
  const bool holdFirstPark_;
  std::array<bool, kMaxWorkers> heldSearch_{};  //!< Each worker accesses only its own entry.
  bool heldPark_ = false;                       //!< Accessed only by worker zero.
  std::array<std::atomic<unsigned>, kMaxWorkers> parkEpoch_{};
};

struct WakePool {
  explicit WakePool(std::size_t workers, unsigned searchHolds = 0, bool holdFirstPark = false)
      : hooks(searchHolds, holdFirstPark),
        scheduler(Peer::Make(TaskSchedulerOptions{.workerCount = workers}, &hooks)),
        roots(*scheduler) {}

  ~WakePool() {
    hooks.release();
    roots.wait();
  }

  bool allParkedExcept(int except) const {
    for (std::size_t i = 0; i < scheduler->workerCount(); ++i) {
      if (static_cast<int>(i) != except && !hooks.parked(*scheduler, i)) {
        return false;
      }
    }
    return Peer::AwakeCount(*scheduler) == (except < 0 ? 0 : 1);
  }

  template <typename Work>
  testing::AssertionResult runOnSoleAwakeWorker(Work work) {
    if (!PollUntil([&] { return allParkedExcept(-1); })) {
      return testing::AssertionFailure() << "workers did not reach their initial park";
    }
    Gate rootReady;
    std::atomic<int> root{-1};
    std::atomic<bool> proceed{false};
    roots.spawn([&] {
      root.store(Peer::WorkerIndex(*scheduler));
      rootReady.arriveAndWait();
      if (proceed.load()) {
        work();
      }
    });
    // Taking even an isolated global root may wake one replacement searcher.
    const bool settled =
        rootReady.waitForArrival() && PollUntil([&] {
          return allParkedExcept(root.load()) && Peer::SearchingCount(*scheduler) == 0;
        });
    proceed.store(settled);
    rootReady.release();
    roots.wait();
    if (!settled || rootReady.heldPastHangGuard()) {
      return testing::AssertionFailure() << "peers did not park while the root was held";
    }
    return testing::AssertionSuccess();
  }

  SearchHooks hooks;
  std::unique_ptr<TaskScheduler> scheduler;
  TaskScope roots;
};

TEST(ParallelismWakeTest, AForkFromTheOnlyAwakeWorkerRunsChildrenOnTwoWorkers) {
  WakePool pool(4);
  std::atomic<int> first{-1};
  std::atomic<int> second{-1};
  std::atomic<unsigned> calls{0};
  Event anotherWorker;
  bool firstSawAnother = false;
  const auto started = pool.runOnSoleAwakeWorker([&] {
    TaskScope children(*pool.scheduler);
    for (int i = 0; i < 64; ++i) {
      children.spawn([&] {
        const int self = Peer::WorkerIndex(*pool.scheduler);
        int expected = -1;
        if (first.compare_exchange_strong(expected, self)) {
          firstSawAnother = anotherWorker.wait();
        } else if (expected != self) {
          second.store(self);
          anotherWorker.set();
        }
        ++calls;
      });
    }
  });
  EXPECT_TRUE(started);
  EXPECT_TRUE(firstSawAnother) << "the first child was blocked until a different worker ran one";
  EXPECT_THAT(second.load(), Ge(0));
  EXPECT_THAT(second.load(), Ne(first.load()));
  EXPECT_THAT(calls.load(), Eq(64u));
  EXPECT_TRUE(PollUntil([&] { return pool.allParkedExcept(-1); }));
  EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0));
}

TEST(ParallelismWakeTest, ForkBurstRecruitsOnlyOneWorkerBeforeItStartsSearching) {
  std::atomic<unsigned> calls{0};
  unsigned wakesDuringFork = 0;
  bool recruitReachedGate = false;
  WakePool pool(4);
  const auto totalWakes = [&] {
    unsigned total = 0;
    for (const auto& count : pool.hooks.wakes) {
      total += count.load();
    }
    return total;
  };

  const auto started = pool.runOnSoleAwakeWorker([&] {
    pool.hooks.hold.arm(SchedulerStep::BeforeSearchStart);
    const unsigned before = totalWakes();
    TaskScope children(*pool.scheduler);
    children.spawn([&] { ++calls; });
    recruitReachedGate = pool.hooks.hold.gate.waitForArrival();
    for (int i = 1; i < 64; ++i) {
      children.spawn([&] { ++calls; });
    }
    wakesDuringFork = totalWakes() - before;
    pool.hooks.hold.gate.release();
  });
  EXPECT_TRUE(started);
  EXPECT_TRUE(recruitReachedGate);
  EXPECT_FALSE(pool.hooks.hold.gate.heldPastHangGuard());
  EXPECT_THAT(wakesDuringFork, Eq(1u));
  EXPECT_THAT(calls.load(), Eq(64u));
  EXPECT_TRUE(PollUntil([&] { return pool.allParkedExcept(-1); }));
  EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0u));
}

TEST(ParallelismWakeTest, LocalForkLeavesDeepWaiterAsleepUntilItsScopeFinishes) {
  SearchHooks hooks(0, false);
  Gate heldTask;
  Event forked;
  std::atomic<int> creator{-1};
  int thief = -1;
  unsigned wakesDuringFork = 0;
  unsigned extraCalls = 0;
  auto scheduler =
      Peer::Make(TaskSchedulerOptions{.workerCount = 2,
                                      .idleSpinRounds = std::numeric_limits<std::size_t>::max(),
                                      .helpMarginBytes = std::numeric_limits<std::size_t>::max()},
                 &hooks);
  TaskScope roots(*scheduler);
  roots.spawn([&] {
    creator.store(Peer::WorkerIndex(*scheduler));
    TaskScope waiting(*scheduler);
    waiting.spawn([&] {
      thief = Peer::WorkerIndex(*scheduler);
      heldTask.arriveAndWait();
      const auto index = static_cast<std::size_t>(creator.load());
      const unsigned before = hooks.wakes[index].load();
      TaskScope extra(*scheduler);
      extra.spawn([&] { ++extraCalls; });
      // Finishing 'waiting' must wake its creator. Count only the optional hint from this push.
      wakesDuringFork = hooks.wakes[index].load() - before;
    });
    forked.set();
    EXPECT_TRUE(heldTask.waitForArrival());
  });
  const bool arranged =
      forked.wait() && heldTask.waitForArrival() && PollUntil([&] {
        const int worker = creator.load();
        return worker >= 0 &&
               hooks.parked(*scheduler, static_cast<std::size_t>(worker), WorkerState::AsleepDeep);
      });
  heldTask.release();
  roots.wait();
  EXPECT_TRUE(arranged);
  EXPECT_FALSE(heldTask.heldPastHangGuard());
  EXPECT_THAT(thief, Ne(creator.load()));
  EXPECT_THAT(wakesDuringFork, Eq(0u));
  EXPECT_THAT(extraCalls, Eq(1u));
}

void CheckRetiringSearcher(unsigned workers) {
  Gate rootReady;
  Gate rootHolding;
  std::atomic<int> childWorker{-1};
  WakePool pool(workers, (1u << workers) - 1);
  for (unsigned i = 0; i < workers; ++i) {
    if (!pool.hooks.firstSearch[i].waitForArrival()) {
      ADD_FAILURE() << "worker " << i << " did not start its first search";
      return;
    }
  }
  pool.roots.spawn([&] {
    rootReady.arriveAndWait();
    TaskScope children(*pool.scheduler);
    children.spawn([&] { childWorker.store(Peer::WorkerIndex(*pool.scheduler)); });
    rootHolding.arriveAndWait();
  });
  pool.hooks.firstSearch[0].release();
  const bool rootStarted = rootReady.waitForArrival();
  pool.hooks.hold.arm(SchedulerStep::AfterEmptySearch, 1);
  pool.hooks.firstSearch[1].release();
  const bool missed = rootStarted && pool.hooks.hold.gate.waitForArrival();
  EXPECT_TRUE(missed);
  if (missed) {
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(workers - 1));
  }
  rootReady.release();
  const bool published = rootHolding.waitForArrival();
  EXPECT_TRUE(published);
  pool.hooks.hold.gate.release();
  // The owner holds the fork, and any other enrolled searcher stays at its gate. Parking here
  // without first taking the fork is the observable failed rescan, even with time to spare.
  const bool decided = missed && published && PollUntil([&] {
                         return childWorker.load() >= 0 || pool.hooks.parked(*pool.scheduler, 1);
                       });
  const int runnerBeforeRecovery = childWorker.load();
  rootHolding.release();
  pool.hooks.release();
  pool.roots.wait();
  EXPECT_TRUE(decided);
  EXPECT_THAT(runnerBeforeRecovery, Eq(1));
  for (std::size_t i = 0; i < pool.scheduler->workerCount(); ++i) {
    EXPECT_FALSE(pool.hooks.firstSearch[i].heldPastHangGuard());
  }
  EXPECT_FALSE(rootReady.heldPastHangGuard());
  EXPECT_FALSE(rootHolding.heldPastHangGuard());
  EXPECT_FALSE(pool.hooks.hold.gate.heldPastHangGuard());
}

TEST(ParallelismWakeTest, LastSearcherRechecksAForkPublishedAfterItsEmptyScan) {
  CheckRetiringSearcher(2);
}

TEST(ParallelismWakeTest, ShallowSearcherRechecksAForkWhileAnotherSearcherIsEnrolled) {
  CheckRetiringSearcher(3);
}

TEST(ParallelismWakeTest, ScopeCompletionRetiresAnUnusedRecruitBeforeTheNextFork) {
  Gate heldChild;
  Gate afterJoin;
  Event firstFork;
  Event secondRan;
  int secondWorker = -1;
  bool sawSecond = false;
  WakePool pool(2, 0b11);
  const bool searchersHeld =
      pool.hooks.firstSearch[0].waitForArrival() && pool.hooks.firstSearch[1].waitForArrival();
  EXPECT_TRUE(searchersHeld);
  if (!searchersHeld) {
    return;
  }
  pool.hooks.hold.arm(SchedulerStep::BeforeSleeperWaits, 0);
  pool.roots.spawn([&] {
    {
      TaskScope waiting(*pool.scheduler);
      waiting.spawn([&] {
        heldChild.arriveAndWait();
        TaskScope fork(*pool.scheduler);
        fork.spawn([] {});  // Recruit the shallow creator before completing its scope.
      });
      firstFork.set();
      EXPECT_TRUE(heldChild.waitForArrival());
    }
    afterJoin.arriveAndWait();
    TaskScope next(*pool.scheduler);
    next.spawn([&] {
      secondWorker = Peer::WorkerIndex(*pool.scheduler);
      secondRan.set();
    });
    sawSecond = secondRan.wait();
  });
  pool.hooks.firstSearch[0].release();
  const bool firstQueued = firstFork.wait();
  pool.hooks.firstSearch[1].release();
  const bool shallow = firstQueued && pool.hooks.hold.gate.waitForArrival();
  EXPECT_TRUE(shallow);
  const unsigned before = pool.hooks.wakes[0].load();
  heldChild.release();
  // The first wake recruits worker zero. The second announces that its scope is complete.
  const bool completed =
      shallow && PollUntil([&] { return pool.hooks.wakes[0].load() >= before + 2; });
  EXPECT_TRUE(completed);
  pool.hooks.hold.gate.release();
  const bool returned = afterJoin.waitForArrival();
  const bool soleAwake = returned && PollUntil([&] { return pool.allParkedExcept(0); });
  EXPECT_TRUE(soleAwake);
  EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0u));
  afterJoin.release();
  pool.roots.wait();
  EXPECT_TRUE(sawSecond);
  EXPECT_THAT(secondWorker, Eq(1));
  EXPECT_FALSE(heldChild.heldPastHangGuard());
  EXPECT_FALSE(afterJoin.heldPastHangGuard());
  EXPECT_FALSE(pool.hooks.hold.gate.heldPastHangGuard());
}

TEST(ParallelismWakeTest, SearchEnrollmentAndRetirementAreIdempotentOnTheOwner) {
  WakePool pool(1);
  const auto started = pool.runOnSoleAwakeWorker([&] {
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0u));
    Peer::StartCurrentSearch(*pool.scheduler);
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(1u));
    Peer::StartCurrentSearch(*pool.scheduler);
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(1u));
    EXPECT_TRUE(Peer::StopCurrentSearch(*pool.scheduler));
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0u));
    EXPECT_FALSE(Peer::StopCurrentSearch(*pool.scheduler));
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0u));
  });
  EXPECT_TRUE(started);
}

TEST(ParallelismWakeTest, LastSearcherTakingWorkWakesAnotherThief) {
  Gate rootHolding;
  Gate firstHolding;
  Event secondStarted;
  WakePool pool(4, 0b0011);
  auto& hooks = pool.hooks;
  const bool searchersHeld =
      hooks.firstSearch[0].waitForArrival() && hooks.firstSearch[1].waitForArrival();
  const bool arranged = searchersHeld && PollUntil([&] {
                          return hooks.parked(*pool.scheduler, 2) &&
                                 hooks.parked(*pool.scheduler, 3) &&
                                 Peer::SearchingCount(*pool.scheduler) == 2;
                        });
  EXPECT_TRUE(arranged) << "two workers must be searching while the other two are parked";
  if (!arranged) {
    return;
  }

  std::atomic<int> first{-1};
  std::atomic<int> second{-1};
  pool.roots.spawn([&] {
    TaskScope children(*pool.scheduler);
    children.spawn([&] {
      first.store(Peer::WorkerIndex(*pool.scheduler));
      firstHolding.arriveAndWait();
    });
    children.spawn([&] {
      second.store(Peer::WorkerIndex(*pool.scheduler));
      secondStarted.set();
    });
    rootHolding.arriveAndWait();
  });
  hooks.firstSearch[0].release();
  const bool forked = rootHolding.waitForArrival();
  EXPECT_TRUE(forked);
  if (forked) {
    EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(1));
    EXPECT_THAT(hooks.wakes[2].load() + hooks.wakes[3].load(), Eq(0u))
        << "neither local push needs to wake a thief while worker one is searching";
  }
  hooks.firstSearch[1].release();
  if (forked) {
    EXPECT_TRUE(firstHolding.waitForArrival());
    EXPECT_TRUE(secondStarted.wait()) << "the thief that claimed the first child needs a successor";
  }
  firstHolding.release();
  rootHolding.release();
  pool.roots.wait();
  EXPECT_FALSE(hooks.firstSearch[0].heldPastHangGuard());
  EXPECT_FALSE(hooks.firstSearch[1].heldPastHangGuard());
  EXPECT_FALSE(firstHolding.heldPastHangGuard());
  EXPECT_FALSE(rootHolding.heldPastHangGuard());
  EXPECT_THAT(first.load(), Eq(1));
  EXPECT_THAT(second.load(), Ge(2));
}

TEST(ParallelismWakeTest, LastSearcherWithAnEmptyRoundParksWithoutRelaying) {
  WakePool pool(2, 0b01, true);
  const bool lastHeld = pool.hooks.firstSearch[0].waitForArrival();
  const bool otherParked =
      lastHeld && PollUntil([&] {
        return pool.hooks.parked(*pool.scheduler, 1) && Peer::SearchingCount(*pool.scheduler) == 1;
      });
  EXPECT_TRUE(otherParked);
  pool.hooks.firstSearch[0].release();
  const bool retiring = pool.hooks.firstPark.waitForArrival();
  EXPECT_TRUE(retiring);
  const bool relayed = pool.hooks.wakes[1].load() != 0;
  EXPECT_FALSE(relayed) << "no task or local publication can justify a replacement wake";
  EXPECT_THAT(Peer::SearchingCount(*pool.scheduler), Eq(0));
  pool.hooks.firstPark.release();
  if (!relayed) {
    EXPECT_TRUE(PollUntil([&] { return pool.allParkedExcept(-1); }));
  }
  EXPECT_FALSE(pool.hooks.firstSearch[0].heldPastHangGuard());
  EXPECT_FALSE(pool.hooks.firstPark.heldPastHangGuard());
}

TEST(ParallelismWakeTest, ShutdownClearsAnActiveSearchWithoutEnteringSleep) {
  struct ShutdownHooks : SchedulerTestHooks {
    void at(SchedulerStep step, std::size_t) override {
      if (step == SchedulerStep::AfterSearchStart) {
        searching.arriveAndWait();
      } else if (step == SchedulerStep::AfterStopFlagSet) {
        stopSeen = true;
        searching.release();
      } else if (step == SchedulerStep::AfterDrain) {
        ++sleeps;
      }
    }

    Gate searching;
    bool stopSeen = false;
    int sleeps = 0;
  } hooks;

  auto scheduler =
      Peer::Make(TaskSchedulerOptions{.workerCount = 1,
                                      .idleSpinRounds = std::numeric_limits<std::size_t>::max()},
                 &hooks);
  const bool started = hooks.searching.waitForArrival();
  const std::size_t searching = Peer::SearchingCount(*scheduler);
  // Shutdown releases the held search and checks the count after joining the worker.
  scheduler.reset();
  EXPECT_TRUE(started);
  EXPECT_THAT(searching, Eq(1u));
  EXPECT_TRUE(hooks.stopSeen);
  EXPECT_FALSE(hooks.searching.heldPastHangGuard());
  EXPECT_THAT(hooks.sleeps, Eq(0));
}

CoroutineTestRoot ResumeLocally(TaskScheduler& scheduler, int& resumedOn) {
  co_await scheduler.schedule();
  resumedOn = Peer::WorkerIndex(scheduler);
}

TEST(ParallelismWakeTest, LocalCoroutineResumptionWakesAParkedWorker) {
  Event done;
  int resumedOn = -1;
  int origin = -1;
  bool completedWhileHeld = false;
  auto pool = std::make_unique<WakePool>(2);
  auto coroutine = ResumeLocally(*pool->scheduler, resumedOn);
  const auto started = pool->runOnSoleAwakeWorker([&] {
    origin = Peer::WorkerIndex(*pool->scheduler);
    coroutine.start(done);
    completedWhileHeld = done.wait();
  });
  EXPECT_TRUE(started);
  EXPECT_TRUE(completedWhileHeld) << "the worker that enqueued the resumption could not run it";
  if (origin >= 0) {
    // A missing wake lets the origin consume its entry after its hold times out.
    const bool finished = done.wait();
    EXPECT_TRUE(finished);
    if (finished) {
      EXPECT_THAT(resumedOn, Ge(0));
      EXPECT_THAT(resumedOn, Ne(origin));
    }
  }
  // Join all workers before the frame can be destroyed, including on a failed completion check.
  pool.reset();
}

}  // namespace
}  // namespace donner::tasks
