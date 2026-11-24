/// @file
/// Scopes and `Join` on workers: the watermark and rules 1 to 3, help within the margin, the
/// asleep deep state, the last-awake rule and the hand-off, and cooperative stop.

#include <alloca.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"
#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {

/// Reaches a scope's count, watermark and link.
class TaskScopeTestPeer {
public:
  static std::uint64_t Count(const TaskScope& scope) {
    return scope.count_.load(std::memory_order_relaxed);
  }

  static std::int64_t Watermark(const TaskScope& scope) { return scope.watermark_; }

  /// Queues \p work as a task of \p scope on the global queue, from a thread that is not a worker.
  template <typename F>
  static bool QueueOnGlobal(TaskScope& scope, F work) {
    return TaskSchedulerTestPeer::TrySpawnLinked(scope.scheduler_, work, &scope.link_);
  }
};

namespace {

using Peer = TaskSchedulerTestPeer;
using ScopePeer = TaskScopeTestPeer;
using testing::Each;
using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::Ne;

/// A margin no worker stack is within: every wait on a worker is past its margin.
constexpr std::size_t kAlwaysPastMargin = std::numeric_limits<std::size_t>::max();

/**
 * Steal rounds before an idle worker sleeps, for cases that need a thief: an idle worker that
 * never sleeps steals a local push at once. The idle worker must be available to steal
 * when its peer pushes onto its own deque.
 */
constexpr std::size_t kNeverSleepIdle = std::numeric_limits<std::size_t>::max();

/// Spins under the hang guard until \p flag is set. @return False if it never was.
bool SpinUntil(const std::atomic<bool>& flag) {
  return PollUntil([&] { return flag.load(); });
}

class WorkerCounts : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(Workers, WorkerCounts, testing::Values(1, 2, 4),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                           return "Workers" + std::to_string(info.param);
                         });

class TwoOrMoreWorkers : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(Workers, TwoOrMoreWorkers, testing::Values(2, 4),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                           return "Workers" + std::to_string(info.param);
                         });

/**
 * Hooks for the worker-wait tests: count every step, hold a chosen worker the next time it reaches
 * a chosen step after the hold is armed (up to four holds at once), and switch the last-awake rule
 * off.
 */
class WaitHooks : public SchedulerTestHooks {
public:
  /// Holds worker \p worker at \p gate the next time it reaches \p step.
  void arm(SchedulerStep step, int worker, Gate* gate) {
    for (Hold& hold : holds_) {
      if (hold.worker.load() == -1 && hold.gate.load() == nullptr) {
        hold.gate.store(gate);
        hold.step.store(static_cast<int>(step));
        hold.worker.store(worker);
        return;
      }
    }
    ADD_FAILURE() << "more than four holds armed";
  }

  void at(SchedulerStep step, std::size_t worker) override {
    counts_[static_cast<std::size_t>(step)].fetch_add(1);
    for (Hold& hold : holds_) {
      int expected = static_cast<int>(worker);
      if (hold.step.load() == static_cast<int>(step) &&
          hold.worker.compare_exchange_strong(expected, -1)) {
        hold.gate.load()->arriveAndWait();
      }
    }
  }

  [[nodiscard]] bool lastAwakeRule() const override { return lastAwakeRule_.load(); }

  void setLastAwakeRule(bool on) { lastAwakeRule_.store(on); }

  [[nodiscard]] int count(SchedulerStep step) const {
    return counts_[static_cast<std::size_t>(step)].load();
  }

private:
  struct Hold {
    std::atomic<int> step{-1};
    std::atomic<int> worker{-1};
    std::atomic<Gate*> gate{nullptr};
  };

  std::array<std::atomic<int>, 32> counts_{};
  std::array<Hold, 4> holds_;
  std::atomic<bool> lastAwakeRule_{true};
};

/// Polls until every worker of \p scheduler is asleep and the awake count agrees with the state
/// words: the quiescent point at which the count is exact.
bool QuiescentAndCounted(const TaskScheduler& scheduler) {
  return PollUntil(
      [&] { return Peer::AwakeStates(scheduler) == 0 && Peer::AwakeCount(scheduler) == 0; });
}

/// Polls until the awake count equals the workers whose state is awake; for schedulers whose idle
/// workers never sleep, once no worker is in a wait.
bool AwakeCountAgrees(const TaskScheduler& scheduler) {
  return PollUntil([&] { return Peer::AwakeCount(scheduler) == Peer::AwakeStates(scheduler); });
}

// Scopes created on workers: every task runs once, including flat spawns from tasks on other
// workers, and each wait sees what its tasks wrote.
TEST_P(WorkerCounts, ScopesOnWorkersRunEveryTaskOnce) {
  constexpr int kRoots = 8;
  constexpr int kChildren = 16;
  constexpr int kGrandchildren = 4;
  std::vector<int> leaves(kRoots * kChildren * kGrandchildren, 0);
  std::vector<int> rootSawAll(kRoots, 0);
  Completion roots;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  for (int r = 0; r < kRoots; ++r) {
    Peer::Spawn(scheduler, [&, r] {
      {
        TaskScope scope(scheduler);
        for (int c = 0; c < kChildren; ++c) {
          scope.spawn([&, r, c] {
            for (int g = 0; g < kGrandchildren; ++g) {
              scope.spawn([&, r, c, g] {
                ++leaves[static_cast<std::size_t>((r * kChildren + c) * kGrandchildren + g)];
              });
            }
          });
        }
        scope.wait();
        const auto first = leaves.begin() + r * kChildren * kGrandchildren;
        rootSawAll[static_cast<std::size_t>(r)] =
            std::all_of(first, first + kChildren * kGrandchildren, [](int n) { return n == 1; });
      }
      roots.arrive();
    });
  }
  ASSERT_TRUE(roots.waitFor(kRoots)) << "a wait on a worker never returned";
  EXPECT_THAT(leaves, Each(Eq(1)));
  EXPECT_THAT(rootSawAll, Each(Eq(1))) << "a wait returned before its tasks' writes were visible";
  EXPECT_TRUE(QuiescentAndCounted(scheduler));
  EXPECT_THAT(Peer::LiveScopes(scheduler), Eq(0u));
}

/// Recurses \p depth levels through nested scopes, each spawning the next level as its one task,
/// and records the worker each level ran on.
void RecurseThroughScopes(TaskScheduler& scheduler, int depth, std::vector<int>& workerAt) {
  if (depth == 0) {
    return;
  }
  TaskScope scope(scheduler);
  scope.spawn([&scheduler, depth, &workerAt] {
    workerAt[static_cast<std::size_t>(depth - 1)] = Peer::WorkerIndex(scheduler);
    RecurseThroughScopes(scheduler, depth - 1, workerAt);
  });
  scope.wait();
}

// One worker recursing through nested scopes, the next level always on its own deque above the
// innermost watermark: rule 1 runs it, so the recursion is the program's own nesting, on the
// worker it started on when no other worker can steal.
TEST_P(WorkerCounts, NestedScopesRecurseThroughRuleOne) {
  constexpr int kDepth = 64;
  std::vector<int> workerAt(kDepth, -1);
  int rootWorker = -1;
  Event done;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  Peer::Spawn(scheduler, [&] {
    rootWorker = Peer::WorkerIndex(scheduler);
    RecurseThroughScopes(scheduler, kDepth, workerAt);
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(workerAt, Each(Ge(0))) << "every level ran on a worker";
  if (GetParam() == 1) {
    EXPECT_THAT(workerAt, Each(Eq(rootWorker)));
  }
  EXPECT_TRUE(QuiescentAndCounted(scheduler));
}

/// Fibonacci through nested `Join`, which forks at every level.
std::uint64_t Fib(TaskScheduler& scheduler, int n) {
  if (n < 2) {
    return static_cast<std::uint64_t>(n);
  }
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  Join(scheduler, [&] { a = Fib(scheduler, n - 1); }, [&] { b = Fib(scheduler, n - 2); });
  return a + b;
}

// Nested `Join` on workers computes the sequential result; the recursion nests on whichever
// worker runs each level.
TEST_P(WorkerCounts, NestedJoinOnWorkersComputesTheSequentialResult) {
  std::uint64_t result = 0;
  Event done;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  Peer::Spawn(scheduler, [&] {
    result = Fib(scheduler, 20);
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(result, Eq(6765u));
  EXPECT_TRUE(QuiescentAndCounted(scheduler));
}

// `Join` on a worker that nobody steals from: `b` is taken back and runs on the caller, after `a`.
TEST(WorkerJoinTest, AnUnstolenJoinRunsBothOnTheCallingWorker) {
  std::vector<char> order;
  std::thread::id aThread;
  std::thread::id bThread;
  std::thread::id caller;
  Event done;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  Peer::Spawn(scheduler, [&] {
    caller = std::this_thread::get_id();
    Join(
        scheduler,
        [&] {
          aThread = std::this_thread::get_id();
          order.push_back('a');
        },
        [&] {
          bThread = std::this_thread::get_id();
          order.push_back('b');
        });
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(order, testing::ElementsAre('a', 'b'));
  EXPECT_THAT(aThread, Eq(caller));
  EXPECT_THAT(bThread, Eq(caller)) << "an unstolen b is taken back by the caller";
}

// `Join` on a worker whose `b` is stolen: the caller waits for it as a scope's creator does.
TEST_P(TwoOrMoreWorkers, AStolenJoinWaitsForTheThief) {
  std::atomic<bool> bStarted{false};
  bool aSawBStart = false;
  int callerWorker = -1;
  int bWorker = -1;
  int bResult = 0;
  int seenResult = 0;
  Event done;
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = GetParam(), .idleSpinRounds = kNeverSleepIdle});
  Peer::Spawn(scheduler, [&] {
    callerWorker = Peer::WorkerIndex(scheduler);
    Join(
        scheduler, [&] { aSawBStart = SpinUntil(bStarted); },
        [&] {
          bWorker = Peer::WorkerIndex(scheduler);
          bStarted.store(true);
          bResult = 42;
        });
    seenResult = bResult;
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_TRUE(aSawBStart) << "an idle worker steals b while a runs";
  EXPECT_THAT(bWorker, Ne(callerWorker));
  EXPECT_THAT(seenResult, Eq(42)) << "the wait sees b's writes";
}

// A stolen task that creates its own scope and waits on it, on the thief.
TEST_P(TwoOrMoreWorkers, AStolenTaskThatWaitsCompletes) {
  constexpr int kLeaves = 32;
  std::atomic<bool> childStarted{false};
  int rootWorker = -1;
  int childWorker = -1;
  std::vector<int> leaves(kLeaves, 0);
  bool childSawAll = false;
  Event done;
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = GetParam(), .idleSpinRounds = kNeverSleepIdle});
  Peer::Spawn(scheduler, [&] {
    rootWorker = Peer::WorkerIndex(scheduler);
    {
      TaskScope scope(scheduler);
      scope.spawn([&] {
        childWorker = Peer::WorkerIndex(scheduler);
        childStarted.store(true);
        TaskScope inner(scheduler);
        for (int i = 0; i < kLeaves; ++i) {
          inner.spawn([&leaves, i] { ++leaves[static_cast<std::size_t>(i)]; });
        }
        inner.wait();
        childSawAll = std::all_of(leaves.begin(), leaves.end(), [](int n) { return n == 1; });
      });
      EXPECT_TRUE(SpinUntil(childStarted)) << "an idle worker steals the child";
      scope.wait();
    }
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(childWorker, Ne(rootWorker));
  EXPECT_TRUE(childSawAll);
  EXPECT_THAT(leaves, Each(Eq(1)));
}

// The watermark: an entry pushed before a scope was created lies below it and is not run by that
// scope's wait, while the scope's own task, pushed at the watermark index itself, is. With one
// worker nothing is stolen, and the wait never sleeps: rule 1 alone completes it.
TEST(WorkerWatermarkTest, AWaitRunsEntriesFromTheWatermarkUpAndNoneBelow) {
  WaitHooks hooks;
  std::string order;
  std::int64_t outerWatermark = -1;
  std::int64_t innerWatermark = -1;
  std::int64_t bottomAfterInnerWait = -1;
  std::string orderAfterInnerWait;
  int drainsDuringInnerWait = -1;
  Event done;
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  Peer::Spawn(*scheduler, [&] {
    {
      TaskScope outer(*scheduler);
      outerWatermark = ScopePeer::Watermark(outer);
      outer.spawn([&] { order += 'x'; });
      {
        TaskScope inner(*scheduler);
        innerWatermark = ScopePeer::Watermark(inner);
        inner.spawn([&] { order += 'c'; });
        const int drainsBefore = hooks.count(SchedulerStep::AfterDrain);
        inner.wait();
        drainsDuringInnerWait = hooks.count(SchedulerStep::AfterDrain) - drainsBefore;
        orderAfterInnerWait = order;
        bottomAfterInnerWait = Peer::DequeBottom(*scheduler);
      }
      outer.wait();
    }
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(innerWatermark, Eq(outerWatermark + 1)) << "the outer scope's task lies below";
  EXPECT_THAT(orderAfterInnerWait, Eq("c")) << "the inner wait runs its entry at the watermark "
                                               "and not the entry just below it";
  EXPECT_THAT(bottomAfterInnerWait, Eq(innerWatermark)) << "the entry below is still queued";
  EXPECT_THAT(drainsDuringInnerWait, Eq(0)) << "rule 1 completed the wait without a sleep";
  EXPECT_THAT(order, Eq("cx")) << "the outer wait then runs the entry above its own watermark";
  scheduler.reset();
}

// The margin test: a waiting worker helps only with more than the margin left.
TEST(WorkerMarginTest, AWaitHelpsOnlyWithMoreThanTheMarginLeft) {
  constexpr std::size_t kMargin = 128 * 1024;
  EXPECT_TRUE(Peer::HelpsWithin(kMargin + 1, kMargin));
  EXPECT_FALSE(Peer::HelpsWithin(kMargin, kMargin)) << "at the margin it does not help";
  EXPECT_FALSE(Peer::HelpsWithin(kMargin - 1, kMargin));
  EXPECT_FALSE(Peer::HelpsWithin(0, 0));
  EXPECT_TRUE(Peer::HelpsWithin(1, 0));
}

/**
 * A root on one worker whose scope's only task is stolen by another worker and held there, so the
 * root's wait finds nothing of its own. Shared with the root and the thief.
 */
struct HeldScope {
  explicit HeldScope(TaskSchedulerOptions options)
      : scheduler(Peer::Make(options, &hooks)), roots(*scheduler) {}

  /// Starts the root, which arms \p step for its own worker just before it waits, and returns once
  /// it has. @return False if the root never got there.
  [[nodiscard]] bool start(SchedulerStep step, Gate* gate) {
    roots.spawn([this, step, gate] {
      rootWorker.store(Peer::WorkerIndex(*scheduler));
      {
        TaskScope scope(*scheduler);
        scope.spawn([this] {
          heldWorker.store(Peer::WorkerIndex(*scheduler));
          held.arriveAndWait();
          // Written after the gate, so only the scope's count orders it before the root's read.
          heldResult = 7;
        });
        if (held.waitForArrival()) {
          hooks.arm(step, rootWorker.load(), gate);
          rootWaiting.set();
          scope.wait();
          seenResult = heldResult;
        }
      }
      rootDone.set();
    });
    return rootWaiting.wait();
  }

  [[nodiscard]] bool rootStateIs(WorkerState expected) const {
    const int root = rootWorker.load();
    return root >= 0 && Peer::State(*scheduler, static_cast<std::size_t>(root)) == expected;
  }

  /// Queues a task from this thread, which is not a worker, recording where it ran.
  void queueGlobalTask() {
    roots.spawn([this] {
      gWorker.store(Peer::WorkerIndex(*scheduler));
      gDone.set();
    });
  }

  WaitHooks hooks;
  std::atomic<int> rootWorker{-1};
  std::atomic<int> heldWorker{-1};
  std::atomic<int> gWorker{-1};
  int heldResult = 0;  //!< Plain: the wait's acquire of the count must publish it to the root.
  int seenResult = 0;  //!< What the root read after its wait; read once rootDone is set.
  Gate held;
  Event rootWaiting;
  Event rootDone;
  Event gDone;
  std::unique_ptr<TaskScheduler> scheduler;
  TaskScope roots;
};

// Within its margin, a waiting worker helps: it takes a global task while its own scope's task is
// held on another worker. The root is held after its drain until the task is queued, so its
// re-check sees the work, it wakes itself, and rule 3 takes the task.
TEST(WorkerMarginTest, WithinItsMarginAWaitingWorkerHelps) {
  Gate drained;
  HeldScope state(TaskSchedulerOptions{
      .workerCount = 2, .idleSpinRounds = kNeverSleepIdle, .helpMarginBytes = 0});
  EXPECT_TRUE(state.start(SchedulerStep::AfterDrain, &drained));
  EXPECT_TRUE(drained.waitForArrival()) << "the root's wait never tried to sleep";
  EXPECT_THAT(state.heldWorker.load(), Ne(state.rootWorker.load()));
  state.queueGlobalTask();
  drained.release();
  EXPECT_TRUE(state.gDone.wait());
  EXPECT_THAT(state.gWorker.load(), Eq(state.rootWorker.load()))
      << "the waiting worker took the global task within its margin, its scope still open";
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(0u));
  // The root then sleeps shallow: it is within its margin.
  EXPECT_TRUE(PollUntil([&] { return state.rootStateIs(WorkerState::AsleepShallow); }));
  state.held.release();
  ASSERT_TRUE(state.rootDone.wait());
  EXPECT_TRUE(AwakeCountAgrees(*state.scheduler));
}

// Past its margin, a waiting worker does not help: with another worker awake it sleeps deep
// although work is queued, and the queued task waits for an awake worker.
TEST(WorkerMarginTest, PastItsMarginAWaitingWorkerSleepsDeepInsteadOfHelping) {
  Gate drained;
  Gate beforeWait;
  HeldScope state(TaskSchedulerOptions{
      .workerCount = 2, .idleSpinRounds = kNeverSleepIdle, .helpMarginBytes = kAlwaysPastMargin});
  EXPECT_TRUE(state.start(SchedulerStep::AfterDrain, &drained));
  EXPECT_TRUE(drained.waitForArrival());
  state.queueGlobalTask();
  state.hooks.arm(SchedulerStep::BeforeSleeperWaits, state.rootWorker.load(), &beforeWait);
  drained.release();
  EXPECT_TRUE(beforeWait.waitForArrival())
      << "with the holder of its task awake, the root must wait despite the queued task";
  EXPECT_TRUE(state.rootStateIs(WorkerState::AsleepDeep));
  EXPECT_THAT(state.gWorker.load(), Eq(-1)) << "nobody ran the queued task: the root is held and "
                                               "the other worker is in the held task";
  beforeWait.release();
  state.held.release();
  ASSERT_TRUE(state.gDone.wait());
  ASSERT_TRUE(state.rootDone.wait());
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(0u))
      << "the root was never the last awake worker";
  EXPECT_TRUE(AwakeCountAgrees(*state.scheduler));
}

/**
 * Roots, one per worker, each with a scope whose only task the test queues on the global queue,
 * then waiting on it past the margin. No public entry point queues a worker's scope's task
 * globally; it is the shape the last-awake rule and the hand-off act on.
 */
struct QueuedScopes {
  QueuedScopes(std::size_t workers, std::size_t roots, std::size_t idleSpinRounds = 1)
      : roots(roots),
        scopes(roots),
        ranOn(roots),
        scheduler(Peer::Make(TaskSchedulerOptions{.workerCount = workers,
                                                  .idleSpinRounds = idleSpinRounds,
                                                  .helpMarginBytes = kAlwaysPastMargin},
                             &hooks)),
        rootTasks(*scheduler) {
    for (std::atomic<int>& worker : ranOn) {
      worker.store(-1);
    }
  }

  /// Starts the roots, each on its own worker when idle workers never sleep, or the one root.
  /// @return False if they did not all start.
  [[nodiscard]] bool start() {
    for (std::size_t i = 0; i < roots; ++i) {
      rootTasks.spawn([this, i] {
        {
          TaskScope scope(*scheduler);
          scopes[i].store(&scope);
          started.fetch_add(1);
          // Keep the published scope alive until the test finishes queueing work into it.
          go.wait(false);
          scope.wait();
        }
        done.arrive();
      });
    }
    const bool allStarted = PollUntil([&] { return started.load() == static_cast<int>(roots); });
    if (!allStarted) {
      letRootsWait();
    }
    return allStarted;
  }

  /// Queues each scope's task on the global queue, then lets the roots wait.
  void queueAndGo(Gate* runningTask = nullptr) {
    for (std::size_t i = 0; i < roots; ++i) {
      const bool queued = ScopePeer::QueueOnGlobal(*scopes[i].load(), [this, i, runningTask] {
        ranOn[i].store(Peer::WorkerIndex(*scheduler));
        if (runningTask != nullptr) {
          runningTask->arriveAndWait();
        }
      });
      EXPECT_TRUE(queued);
    }
    letRootsWait();
  }

  void letRootsWait() {
    go.store(true);
    go.notify_all();
  }

  /// Before any publication, every worker has an empty queue and can only enter its first sleep.
  [[nodiscard]] bool initialWorkersParked() const {
    return PollUntil([&] {
      return hooks.count(SchedulerStep::BeforeSleeperWaits) ==
             static_cast<int>(scheduler->workerCount());
    });
  }

  /// Waits for every root. On failure, turns the last-awake rule back on and wakes a worker, so
  /// the scheduler can still finish and be destroyed. @return Whether they finished unaided.
  [[nodiscard]] bool finish() {
    const bool finished = done.waitFor(static_cast<int>(roots));
    if (!finished) {
      hooks.setLastAwakeRule(true);
      rootTasks.spawn([] {});  // Publishes with every worker asleep: wakes one.
      EXPECT_TRUE(done.waitFor(static_cast<int>(roots))) << "even the recovery did not finish";
    }
    return finished;
  }

  const std::size_t roots;
  WaitHooks hooks;
  std::vector<std::atomic<TaskScope*>> scopes;
  std::vector<std::atomic<int>> ranOn;
  std::atomic<int> started{0};
  std::atomic<bool> go{false};
  Completion done;
  std::unique_ptr<TaskScheduler> scheduler;
  TaskScope rootTasks;
};

// Every worker waits past its margin on a scope whose task is queued: the last awake worker, and
// only it, runs queued work past its margin; that completes a scope, whose woken creator returns to
// its idle loop and runs the rest from the top of its stack.
TEST_P(WorkerCounts, EveryWorkerPastItsMarginWithWorkQueuedTheLastAwakeRunsIt) {
  // Idle workers never sleep, so each root lands on its own worker; every worker then holds a
  // root until the first scope completes.
  QueuedScopes state(GetParam(), GetParam(), kNeverSleepIdle);
  ASSERT_TRUE(state.start());
  state.queueAndGo();
  ASSERT_TRUE(state.finish()) << "every worker slept past its margin with work queued";
  for (const std::atomic<int>& worker : state.ranOn) {
    EXPECT_THAT(worker.load(), Ge(0));
  }
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(1u))
      << "exactly one task ran past a margin: the last awake worker's";
  EXPECT_TRUE(AwakeCountAgrees(*state.scheduler));
}

// The same with the last-awake rule switched off through the test hook: every worker sleeps with
// work queued, so the case fails until the recovery turns the rule back on. Kept as a negative
// control of the case above.
TEST_P(WorkerCounts, WithoutTheLastAwakeRuleQueuedWorkWaitsOnASleepingPool) {
  QueuedScopes state(GetParam(), GetParam(), kNeverSleepIdle);
  state.hooks.setLastAwakeRule(false);
  ASSERT_TRUE(state.start());
  state.queueAndGo();
  // Every root sleeps deep and nobody runs the queued tasks.
  EXPECT_TRUE(PollUntil([&] {
    return state.hooks.count(SchedulerStep::BeforeSleeperWaits) == static_cast<int>(GetParam()) &&
           Peer::AwakeStates(*state.scheduler) == 0 &&
           Peer::GlobalQueued(*state.scheduler) == static_cast<std::int64_t>(GetParam());
  }));
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(0u));
  state.hooks.setLastAwakeRule(true);
  state.rootTasks.spawn([] {});  // Publishes with every worker asleep: wakes one.
  ASSERT_TRUE(state.done.waitFor(static_cast<int>(GetParam())));
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(1u));
}

// The hand-off: the last awake worker, waiting past its margin with work queued, wakes a worker
// asleep shallow instead of running the work on its deep stack, and the shallow worker runs it.
TEST_P(TwoOrMoreWorkers, TheLastAwakeWorkerPastItsMarginHandsWorkToAShallowSleeper) {
  QueuedScopes state(GetParam(), 1);
  ASSERT_TRUE(state.initialWorkersParked());
  ASSERT_TRUE(state.start());
  state.queueAndGo();
  ASSERT_TRUE(state.finish()) << "the last awake worker slept without handing the work off";
  EXPECT_THAT(state.ranOn[0].load(), Ge(0));
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(0u))
      << "the shallow worker ran the task, not the deep one";
  EXPECT_THAT(state.hooks.count(SchedulerStep::BeforeHandOff), Eq(1));
  EXPECT_TRUE(QuiescentAndCounted(*state.scheduler));
}

// The drain: a waiting worker that sleeps moves the entries below its watermark to the global
// queue. Valid programs never leave an entry there when a wait sleeps (thieves take the oldest
// entry first, and a scope's tasks leave only after everything older), so the root pushes an older
// task and the test peer publishes the inner scope's task directly to the global queue.
TEST(WorkerDrainTest, DrainingBelowTheWatermarkPreservesTheWorkerRecordBound) {
  WaitHooks hooks;
  Gate drained;
  std::atomic<TaskScope*> scopePtr{nullptr};
  Event created;
  std::atomic<bool> go{false};
  Event rootDone;
  Event scopeTaskDone;
  Event belowDone;
  std::atomic<bool> rootReturned{false};
  std::atomic<bool> belowRanAfterRoot{false};
  bool extraRan = false;
  constexpr std::size_t kBound = 1;
  auto scheduler = Peer::Make(
      TaskSchedulerOptions{
          .workerCount = 1, .maxTasksPerCreator = kBound, .helpMarginBytes = kAlwaysPastMargin},
      &hooks);
  TaskScope roots(*scheduler);
  roots.spawn([&] {
    roots.spawn([&] {
      belowRanAfterRoot.store(rootReturned.load());
      belowDone.set();
    });
    {
      TaskScope scope(*scheduler);
      scopePtr.store(&scope);
      hooks.arm(SchedulerStep::AfterDrain, Peer::WorkerIndex(*scheduler), &drained);
      created.set();
      go.wait(false);
      scope.wait();
    }
    // The deque is empty, but the drained record still belongs to this worker's pool.
    EXPECT_THAT(Peer::WorkerLiveRecords(*scheduler), Eq(kBound));
    roots.spawn([&] {
      EXPECT_THAT(Peer::WorkerLiveRecords(*scheduler), Eq(kBound));
      extraRan = true;
    });
    EXPECT_TRUE(extraRan) << "the worker at its record bound must run the extra spawn inline";
    rootReturned.store(true);
    rootDone.set();
  });
  const bool scopeReady = created.wait();
  EXPECT_TRUE(scopeReady);
  const bool queued =
      scopeReady && ScopePeer::QueueOnGlobal(*scopePtr.load(), [&] { scopeTaskDone.set(); });
  EXPECT_TRUE(queued);
  go.store(true);
  go.notify_one();
  EXPECT_TRUE(drained.waitForArrival());
  EXPECT_THAT(Peer::GlobalQueued(*scheduler), Eq(2))
      << "the drain moved the entry below the watermark to the global queue";
  drained.release();
  ASSERT_TRUE(rootDone.wait());
  ASSERT_TRUE(belowDone.wait());
  if (queued) {
    EXPECT_TRUE(scopeTaskDone.wait());
  }
  EXPECT_THAT(Peer::PastMarginRuns(*scheduler), Eq(1u))
      << "the only worker, last awake, ran the scope's task (first in) past its margin";
  EXPECT_TRUE(belowRanAfterRoot.load()) << "the drained entry ran after the root returned";
}

// Deterministic interleavings of a waiting worker's sleep with its scope's last finisher. The
// root's only task is held on the other worker, whose idle loop never sleeps, so every protocol
// step counted below is the root's.

/// The finisher cannot wake the creator until the creator has read the task's result.
/// A relaxed read of the actual count lets the creator observe zero without acquiring the result.
class FinishBeforeWorkerRecheck : public SchedulerTestHooks {
public:
  void at(SchedulerStep step, std::size_t worker) override {
    if (step == SchedulerStep::AfterSleeperDecrement && waiting[worker] != nullptr) {
      stolen.release();
      sawDecrement = PollUntil([&] { return ScopePeer::Count(*waiting[worker]) == 0; });
    } else if (step == SchedulerStep::ScopeFinisherAfterDecrement) {
      beforeWake.arriveAndWait();
    }
  }

  // Each slot is accessed only by its worker while that worker owns the scope.
  std::array<const TaskScope*, 2> waiting{};
  Gate stolen;
  bool sawDecrement = false;
  Gate beforeWake;
};

TEST(WorkerInterleavingTest, ZeroCountRecheckPublishesResultBeforeFinisherWakesCreator) {
  FinishBeforeWorkerRecheck hooks;
  Event joined;
  int result = 0;
  int observed = 0;
  bool sawSteal = false;
  auto scheduler =
      Peer::Make(TaskSchedulerOptions{.workerCount = 2, .idleSpinRounds = kNeverSleepIdle}, &hooks);
  bool completed;
  {
    TaskScope roots(*scheduler);
    roots.spawn([&] {
      {
        TaskScope scope(*scheduler);
        scope.spawn([&] {
          hooks.stolen.arriveAndWait();
          result = 41;
        });
        sawSteal = hooks.stolen.waitForArrival();
        const auto worker = static_cast<std::size_t>(Peer::WorkerIndex(*scheduler));
        hooks.waiting[worker] = &scope;
        scope.wait();
        hooks.waiting[worker] = nullptr;
        observed = result;
        hooks.beforeWake.release();
      }
      joined.set();
    });

    completed = joined.wait();
    hooks.stolen.release();
    hooks.beforeWake.release();
  }
  scheduler.reset();
  EXPECT_TRUE(completed);
  EXPECT_TRUE(sawSteal);
  EXPECT_FALSE(hooks.stolen.heldPastHangGuard());
  EXPECT_TRUE(hooks.sawDecrement);
  EXPECT_FALSE(hooks.beforeWake.heldPastHangGuard());
  EXPECT_THAT(observed, Eq(41));
}

// The scope completes between the root's decrement and its re-check: the finisher's swap finds the
// root asleep and counts it awake, and the re-check sees the zero count, so the root does not wait
// and its own swap fails.
TEST(WorkerInterleavingTest, ACompletionBetweenTheDecrementAndTheRecheckIsSeenByTheRecheck) {
  Gate decremented;
  HeldScope state(TaskSchedulerOptions{.workerCount = 2, .idleSpinRounds = kNeverSleepIdle});
  EXPECT_TRUE(state.start(SchedulerStep::AfterSleeperDecrement, &decremented));
  EXPECT_TRUE(decremented.waitForArrival());
  EXPECT_THAT(Peer::AwakeCount(*state.scheduler), Eq(1)) << "the root counted itself asleep";
  state.held.release();
  EXPECT_TRUE(PollUntil([&] { return Peer::AwakeCount(*state.scheduler) == 2; }))
      << "the finisher's swap counts the asleep root awake";
  decremented.release();
  ASSERT_TRUE(state.rootDone.wait());
  EXPECT_THAT(state.seenResult, Eq(7)) << "the re-check's acquire publishes the task's write";
  EXPECT_THAT(state.hooks.count(SchedulerStep::BeforeSleeperWaits), Eq(0))
      << "the re-check saw the zero count";
  EXPECT_TRUE(AwakeCountAgrees(*state.scheduler)) << "the root was counted awake once";
}

// The scope completes after the root's re-check, before its wait: the finisher's swap and parker
// change end the wait, and the root's own swap after the wait fails.
TEST(WorkerInterleavingTest, ACompletionAfterTheRecheckEndsTheWait) {
  Gate beforeWait;
  HeldScope state(TaskSchedulerOptions{.workerCount = 2, .idleSpinRounds = kNeverSleepIdle});
  EXPECT_TRUE(state.start(SchedulerStep::BeforeSleeperWaits, &beforeWait));
  EXPECT_TRUE(beforeWait.waitForArrival());
  state.held.release();
  EXPECT_TRUE(PollUntil([&] { return Peer::AwakeCount(*state.scheduler) == 2; }));
  beforeWait.release();
  ASSERT_TRUE(state.rootDone.wait()) << "the finisher's change must end the root's wait";
  EXPECT_THAT(state.seenResult, Eq(7));
  EXPECT_TRUE(AwakeCountAgrees(*state.scheduler));
}

// The root wakes itself on seeing the zero count while the finisher, having won its swap, is held
// before its parker change: exactly one swap succeeds, so the root is counted awake once, and the
// late parker change is a spurious wake for whatever the root waits on next.
TEST(WorkerInterleavingTest, ASelfWakeRacingTheFinishersSwapCountsTheRootOnce) {
  Gate decremented;
  Gate finisherSwapped;
  HeldScope state(TaskSchedulerOptions{.workerCount = 2, .idleSpinRounds = kNeverSleepIdle});
  EXPECT_TRUE(state.start(SchedulerStep::AfterSleeperDecrement, &decremented));
  EXPECT_TRUE(decremented.waitForArrival());
  // The step is reported with the woken worker's index.
  state.hooks.arm(SchedulerStep::BetweenWakeSwapAndParkerChange, state.rootWorker.load(),
                  &finisherSwapped);
  state.held.release();
  EXPECT_TRUE(finisherSwapped.waitForArrival());
  EXPECT_TRUE(state.rootStateIs(WorkerState::Awake)) << "the finisher won the swap";
  decremented.release();
  EXPECT_TRUE(state.rootDone.wait()) << "the root's re-check sees zero and returns";
  finisherSwapped.release();
  EXPECT_TRUE(AwakeCountAgrees(*state.scheduler)) << "the root's own swap must not count again";
}

// The hand-off races the shallow worker's own wake: the deep root is held after deciding to hand
// off, while a publish has already woken the shallow worker. The root's swap then fails, and the
// shallow worker, counted once, still runs the queued task.
TEST(WorkerInterleavingTest, AHandOffRacingTheShallowWorkersWakeCountsItOnce) {
  Gate handOff;
  Gate runningTask;
  Event extraRan;
  QueuedScopes state(2, 1);
  ASSERT_TRUE(state.initialWorkersParked());
  ASSERT_TRUE(state.start());
  int rootWorker = -1;
  for (std::size_t i = 0; i < 2; ++i) {
    if (Peer::State(*state.scheduler, i) == WorkerState::Awake) {
      rootWorker = static_cast<int>(i);
    }
  }
  state.hooks.arm(SchedulerStep::BeforeHandOff, rootWorker, &handOff);
  state.queueAndGo(&runningTask);
  EXPECT_TRUE(handOff.waitForArrival());
  // The root is counted asleep; a publish now reads no worker awake and wakes the shallow one.
  EXPECT_THAT(Peer::AwakeCount(*state.scheduler), Eq(0));
  state.rootTasks.spawn([&] { extraRan.set(); });
  EXPECT_TRUE(runningTask.waitForArrival());
  const int beforeHandOff = state.hooks.count(SchedulerStep::BeforeSleeperWaits);
  handOff.release();
  EXPECT_TRUE(PollUntil(
      [&] { return state.hooks.count(SchedulerStep::BeforeSleeperWaits) > beforeHandOff; }));
  EXPECT_THAT(Peer::AwakeCount(*state.scheduler), Eq(1));
  runningTask.release();
  ASSERT_TRUE(state.finish());
  EXPECT_TRUE(extraRan.wait());
  EXPECT_THAT(Peer::PastMarginRuns(*state.scheduler), Eq(0u));
  EXPECT_TRUE(QuiescentAndCounted(*state.scheduler)) << "the shallow worker was counted once";
}

/// Recurses through nested `Join`, one level per `a` with a leaf as `b`, with \p frameBytes
/// frames until less than \p stopBelow bytes of stack are left, and records the least stack left
/// seen.
void JoinDown(TaskScheduler& scheduler, std::size_t frameBytes, std::size_t stopBelow,
              std::atomic<std::size_t>& leastLeft) {
  auto* frame = static_cast<volatile char*>(alloca(frameBytes));
  for (std::size_t i = 0; i < frameBytes; i += 512) {
    frame[i] = static_cast<char>(i);
  }
  const std::size_t left = Peer::StackRemaining().value_or(0);
  std::size_t seen = leastLeft.load();
  while (left < seen && !leastLeft.compare_exchange_weak(seen, left)) {}
  if (left < stopBelow) {
    return;
  }
  std::atomic<int> leaf{0};
  Join(
      scheduler, [&] { JoinDown(scheduler, frameBytes, stopBelow, leastLeft); },
      [&] { leaf.fetch_add(1); });
  EXPECT_THAT(leaf.load(), Eq(1));
}

// Nested `Join` down past the margin: past it a wait no longer helps, but rule 1 keeps running the
// recursion's own entries, so the recursion completes on whatever stacks it reached, and no task
// runs past a margin through the last-awake rule.
TEST_P(WorkerCounts, NestedJoinRecursesDownPastTheMargin) {
  constexpr std::size_t kStack = 512 * 1024;
  constexpr std::size_t kMargin = 256 * 1024;
  std::atomic<std::size_t> leastLeft{kStack};
  Event done;
  TaskScheduler scheduler(TaskSchedulerOptions{
      .workerCount = GetParam(), .workerStackBytes = kStack, .helpMarginBytes = kMargin});
  Peer::Spawn(scheduler, [&] {
    JoinDown(scheduler, 8 * 1024, kMargin / 2, leastLeft);
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(leastLeft.load(), testing::Lt(kMargin)) << "the recursion went past the margin";
  EXPECT_THAT(Peer::PastMarginRuns(scheduler), Eq(0u));
  EXPECT_TRUE(QuiescentAndCounted(scheduler));
}

// A finisher's swap fails just before the creator's step 3: a worker waits on an inner scope inside
// an outer one and is held between its waiting bit and its asleep store while the outer scope's
// last task finishes. The finisher's swap finds the worker awake and fails, its parker change
// lands, and the worker then stores asleep and decrements; its wait returns at once, and only its
// self-wake swap counts it awake again. The counts must agree at quiescence, and a following
// schedule with every worker past its margin must complete.
TEST(WorkerInterleavingTest, AFinishersFailedSwapBeforeTheCreatorsAsleepStoreCountsItOnce) {
  // Three workers: the root, and one idle worker to steal each of its two tasks.
  Gate outerTask;
  Gate innerTask;
  Gate waitingBit;
  std::atomic<int> rootWorker{-1};
  Event rootDone;
  QueuedScopes state(3, 3, kNeverSleepIdle);
  TaskScheduler& scheduler = *state.scheduler;
  WaitHooks& hooks = state.hooks;
  state.rootTasks.spawn([&] {
    rootWorker.store(Peer::WorkerIndex(scheduler));
    {
      TaskScope outer(scheduler);
      outer.spawn([&] { outerTask.arriveAndWait(); });
      // An idle worker steals each task before the root waits, so the root finds nothing of its
      // own: the outer task first (the oldest entry), then the inner one.
      if (outerTask.waitForArrival()) {
        TaskScope inner(scheduler);
        inner.spawn([&] { innerTask.arriveAndWait(); });
        if (innerTask.waitForArrival()) {
          hooks.arm(SchedulerStep::AfterWaitingBit, rootWorker.load(), &waitingBit);
          inner.wait();
        }
      }
    }
    rootDone.set();
  });
  const bool reachedWaitingBit = waitingBit.waitForArrival();
  EXPECT_TRUE(reachedWaitingBit) << "the root never tried to sleep in its inner wait";
  if (reachedWaitingBit) {
    const ParkerEpochWatch rootParker(scheduler, static_cast<std::size_t>(rootWorker.load()));
    const std::uint64_t epochBefore = rootParker.load();
    const int swapsBefore = hooks.count(SchedulerStep::BetweenWakeSwapAndParkerChange);
    outerTask.release();  // The outer scope's last task finishes: its finisher wakes the root.
    EXPECT_TRUE(PollUntil([&] { return rootParker.load() != epochBefore; }))
        << "the finisher changes the root's parker";
    EXPECT_THAT(hooks.count(SchedulerStep::BetweenWakeSwapAndParkerChange), Gt(swapsBefore));
    EXPECT_THAT(Peer::State(scheduler, static_cast<std::size_t>(rootWorker.load())),
                Eq(WorkerState::Awake))
        << "the finisher's swap failed: the root had not stored asleep";
    const int firstWait = hooks.count(SchedulerStep::AfterWaitingBit);
    waitingBit.release();
    EXPECT_TRUE(PollUntil([&] { return hooks.count(SchedulerStep::AfterWaitingBit) > firstWait; }))
        << "the outer completion changed the first token while the inner task was still held";
  } else {
    outerTask.release();
    waitingBit.release();
  }
  innerTask.release();
  ASSERT_TRUE(rootDone.wait());
  EXPECT_TRUE(AwakeCountAgrees(scheduler))
      << "the awake count must equal the workers whose state is awake";

  // A following schedule with every worker past its margin completes.
  ASSERT_TRUE(state.start());
  state.queueAndGo();
  EXPECT_TRUE(state.finish());
  EXPECT_TRUE(AwakeCountAgrees(scheduler));
}

/// Counts destructions, so a closure's lifetime is visible after the task ran or was skipped.
struct Tracker {
  std::atomic<int>* destroyed;
  ~Tracker() { destroyed->fetch_add(1); }
};

/// A closure that owns a \ref Tracker.
struct TrackedClosure {
  std::unique_ptr<Tracker> tracker;
  std::atomic<int>* runs;
  void operator()() const { runs->fetch_add(1); }
};

// Stop before the tasks start: a worker's scope requests a stop with its tasks still on its deque;
// its wait takes each, destroys it unrun, and still counts it finished.
TEST(WorkerStopTest, AStopBeforeTheTasksStartSkipsEveryOne) {
  constexpr int kTasks = 3;
  std::atomic<int> runs{0};
  std::atomic<int> destroyed{0};
  bool stopSeen = false;
  std::uint64_t countBeforeWait = 0;
  Event done;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  Peer::Spawn(scheduler, [&] {
    {
      TaskScope scope(scheduler);
      for (int i = 0; i < kTasks; ++i) {
        scope.spawn(TrackedClosure{std::unique_ptr<Tracker>(new Tracker{&destroyed}), &runs});
      }
      countBeforeWait = ScopePeer::Count(scope);
      scope.requestStop();
      stopSeen = scope.stopRequested();
      scope.wait();
    }
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(countBeforeWait, Eq(static_cast<std::uint64_t>(kTasks)));
  EXPECT_TRUE(stopSeen);
  EXPECT_THAT(runs.load(), Eq(0)) << "a stopped scope's queued tasks do not run";
  EXPECT_THAT(destroyed.load(), Eq(kTasks)) << "their closures are destroyed unrun";
}

// Stop before the tasks start, from a thread that is not a worker: its queued tasks on the global
// queue are skipped by whichever worker takes them, and the wait returns.
TEST(WorkerStopTest, ANonWorkerScopesStopSkipsItsQueuedTasks) {
  constexpr int kTasks = 3;
  Gate busy;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  Peer::Spawn(scheduler, [&] { busy.arriveAndWait(); });
  EXPECT_TRUE(busy.waitForArrival());
  std::atomic<int> runs{0};
  std::atomic<int> destroyed{0};
  {
    TaskScope scope(scheduler);
    for (int i = 0; i < kTasks; ++i) {
      scope.spawn(TrackedClosure{std::unique_ptr<Tracker>(new Tracker{&destroyed}), &runs});
    }
    scope.requestStop();
    busy.release();
    scope.wait();
  }
  EXPECT_THAT(runs.load(), Eq(0));
  EXPECT_THAT(destroyed.load(), Eq(kTasks));
  EXPECT_THAT(Peer::ExternalLiveRecords(scheduler), Eq(0u)) << "skipped records are returned";
}

// Stop during a task: a running task polls the request and returns early; the tasks still queued
// behind it on the only worker are skipped.
TEST(WorkerStopTest, AStopDuringATaskEndsItsPollAndSkipsTheQueuedRest) {
  constexpr int kQueued = 4;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  std::atomic<bool> pollerStarted{false};
  std::atomic<bool> pollerSawStop{false};
  std::atomic<int> runs{0};
  std::atomic<int> destroyed{0};
  {
    TaskScope scope(scheduler);
    scope.spawn([&] {
      pollerStarted.store(true);
      pollerSawStop.store(PollUntil([&] { return scope.stopRequested(); }));
    });
    for (int i = 0; i < kQueued; ++i) {
      scope.spawn(TrackedClosure{std::unique_ptr<Tracker>(new Tracker{&destroyed}), &runs});
    }
    EXPECT_TRUE(SpinUntil(pollerStarted));
    scope.requestStop();
    scope.wait();
  }
  EXPECT_TRUE(pollerSawStop.load()) << "a running task sees the request when it polls";
  EXPECT_THAT(runs.load(), Eq(0)) << "the tasks queued behind it were skipped";
  EXPECT_THAT(destroyed.load(), Eq(kQueued));
}

// Stop after the tasks ran: nothing is undone and the wait returns; a stop concerns only its own
// scope, so a scope created inside a stopped scope's task runs its tasks; and a spawn that runs
// inline is not queued, so it runs even after a stop.
TEST(WorkerStopTest, AStopAfterwardsOrInAnotherScopeChangesNothingElse) {
  std::atomic<int> runs{0};
  std::atomic<int> innerRuns{0};
  Event done;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 2});
  Peer::Spawn(scheduler, [&] {
    {
      TaskScope outer(scheduler);
      outer.spawn([&] {
        runs.fetch_add(1);
        outer.requestStop();
        TaskScope inner(scheduler);
        for (int i = 0; i < 4; ++i) {
          inner.spawn([&] { innerRuns.fetch_add(1); });
        }
        inner.wait();
      });
      outer.wait();
      outer.requestStop();  // After every task ran.
      outer.wait();
    }
    done.set();
  });
  ASSERT_TRUE(done.wait());
  EXPECT_THAT(runs.load(), Eq(1));
  EXPECT_THAT(innerRuns.load(), Eq(4)) << "the inner scope has its own request";

  TaskScheduler inlineScheduler(TaskSchedulerOptions{.workerCount = 0});
  int inlineRuns = 0;
  TaskScope scope(inlineScheduler);
  scope.requestStop();
  scope.spawn([&] { ++inlineRuns; });
  EXPECT_THAT(inlineRuns, Eq(1)) << "an inline spawn is not queued and is not skipped";
}

/**
 * A help ping-pong between two workers. At each level the worker forks with `Join`; `a` waits
 * until the other worker has stolen `b` and has itself forked the next level, so when `a` returns,
 * the other worker's next `b` is queued and this worker's wait helps by stealing it. Each worker's
 * stack then grows by one help per two levels. Without that handshake a wait that finds nothing
 * sleeps, and a worker's push onto its own deque wakes no sleeper.
 */
struct HelpPingPong {
  HelpPingPong(TaskScheduler& scheduler, std::size_t frameBytes, int levels)
      : scheduler(scheduler),
        frameBytes(frameBytes),
        levels(levels),
        forked(static_cast<std::size_t>(levels)),
        remainingAt(static_cast<std::size_t>(levels), 0) {}

  /// Runs level \p depth on the calling worker.
  void level(int depth) {
    if (frameBytes != 0) {
      auto* frame = static_cast<volatile char*>(alloca(frameBytes));
      for (std::size_t i = 0; i < frameBytes; i += 512) {
        frame[i] = static_cast<char>(i);
      }
    }
    const auto index = static_cast<std::size_t>(depth);
    remainingAt[index] = Peer::StackRemaining().value_or(0);
    if (depth + 1 == levels) {
      forked[index].store(true);
      return;
    }
    Join(
        scheduler,
        [&] {
          forked[index].store(true);
          (void)SpinUntil(forked[index + 1]);  // b was stolen and has forked in turn.
        },
        [&] { level(depth + 1); });
  }

  TaskScheduler& scheduler;
  const std::size_t frameBytes;
  const int levels;
  std::vector<std::atomic<bool>> forked;
  std::vector<std::size_t> remainingAt;
};

// Recursion and stack use: through rule 1 (nested scopes on one worker), each level costs a few
// hundred bytes; through helping (the ping-pong), each help costs a level's frames plus a wait's.
// The figures are printed for the build being tested, and only their order is asserted.
TEST(WorkerStackUseTest, NestingThroughRuleOneAndThroughHelpingIsMeasured) {
  constexpr int kLevels = 16;
  TaskScheduler scheduler(TaskSchedulerOptions{
      .workerCount = 2, .idleSpinRounds = kNeverSleepIdle, .helpMarginBytes = 0});
  HelpPingPong game(scheduler, 0, kLevels);
  Event done;
  TaskScope roots(scheduler);
  roots.spawn([&] {
    game.level(0);
    done.set();
  });
  ASSERT_TRUE(done.wait());
  const std::vector<std::size_t>& pingPong = game.remainingAt;
  // Levels alternate between the two workers; consecutive levels on one worker differ by one help.
  std::vector<std::size_t> perHelp;
  for (int level = 2; level < kLevels; ++level) {
    const std::size_t before = pingPong[static_cast<std::size_t>(level - 2)];
    const std::size_t after = pingPong[static_cast<std::size_t>(level)];
    ASSERT_THAT(before, Gt(after)) << "each help nests deeper on its worker, level " << level;
    perHelp.push_back(before - after);
  }
  std::cout << "Stack per help (bytes):";
  for (const std::size_t bytes : perHelp) {
    std::cout << ' ' << bytes;
  }
  std::cout << '\n';

  std::vector<std::size_t> nested(9, 0);
  Event nestedDone;
  TaskScheduler one(TaskSchedulerOptions{.workerCount = 1});
  Peer::Spawn(one, [&] {
    std::function<void(int)> level = [&](int depth) {
      nested[static_cast<std::size_t>(depth)] = Peer::StackRemaining().value_or(0);
      if (depth + 1 == static_cast<int>(nested.size())) {
        return;
      }
      TaskScope scope(one);
      scope.spawn([&, depth] { level(depth + 1); });
      scope.wait();
    };
    level(0);
    nestedDone.set();
  });
  ASSERT_TRUE(nestedDone.wait());
  std::cout << "Stack per nested scope through rule 1 (bytes):";
  for (std::size_t depth = 1; depth < nested.size(); ++depth) {
    ASSERT_THAT(nested[depth - 1], Gt(nested[depth]));
    std::cout << ' ' << nested[depth - 1] - nested[depth];
  }
  std::cout << '\n';
}

// The stack-floor check is reachable in production: helping nests one stolen task per level on a
// waiting worker, and with large frames and a small stack, the ping-pong runs a worker down to its
// reserve. The check then stops the process with its named failure before the stack overflows.
TEST(WorkerStackDeathTest, HelpingDownToTheReserveFailsTheFloorCheck) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 2,
                                                     .idleSpinRounds = kNeverSleepIdle,
                                                     .workerStackBytes = 256 * 1024,
                                                     .helpMarginBytes = 0});
        HelpPingPong game(scheduler, 16 * 1024, 1000);
        Event done;
        TaskScope root(scheduler);
        root.spawn([&] {
          game.level(0);
          done.set();
        });
        (void)done.wait();
      },
      "out of stack");
}

// Stress: fork-join trees through nested scopes and `Join`, with every wait within its margin and
// with every wait past it, without hooks and with short random delays at every protocol step.

struct WorkerStressForm {
  std::size_t workers;
  bool pastMargin;
  bool delays;
  int rounds;
};

class WorkerScopeStressTest : public testing::TestWithParam<WorkerStressForm> {};

INSTANTIATE_TEST_SUITE_P(
    Forms, WorkerScopeStressTest,
    testing::Values(WorkerStressForm{1, false, false, 200}, WorkerStressForm{2, false, false, 200},
                    WorkerStressForm{4, false, false, 200}, WorkerStressForm{2, true, false, 200},
                    WorkerStressForm{4, true, false, 200}, WorkerStressForm{2, false, true, 50},
                    WorkerStressForm{4, false, true, 50}, WorkerStressForm{4, true, true, 50}),
    [](const testing::TestParamInfo<WorkerStressForm>& info) {
      return "Workers" + std::to_string(info.param.workers) +
             (info.param.pastMargin ? "PastMargin" : "WithinMargin") +
             (info.param.delays ? "InjectedDelay" : "NoHooks");
    });

/// Sums [first, last) by splitting in halves through `Join` and, every other level, a scope.
std::uint64_t TreeSum(TaskScheduler& scheduler, std::uint64_t first, std::uint64_t last,
                      int level) {
  if (last - first <= 8) {
    std::uint64_t sum = 0;
    for (std::uint64_t i = first; i < last; ++i) {
      sum += i;
    }
    return sum;
  }
  const std::uint64_t mid = first + (last - first) / 2;
  std::uint64_t left = 0;
  std::uint64_t right = 0;
  if (level % 2 == 0) {
    Join(
        scheduler, [&] { left = TreeSum(scheduler, first, mid, level + 1); },
        [&] { right = TreeSum(scheduler, mid, last, level + 1); });
  } else {
    TaskScope scope(scheduler);
    scope.spawn([&] { left = TreeSum(scheduler, first, mid, level + 1); });
    scope.spawn([&] { right = TreeSum(scheduler, mid, last, level + 1); });
    scope.wait();
  }
  return left + right;
}

TEST_P(WorkerScopeStressTest, ForkJoinTreesComplete) {
  static DelayHooks delays;
  const WorkerStressForm form = GetParam();
  constexpr std::uint64_t kSize = 2048;
  constexpr std::uint64_t kExpected = kSize * (kSize - 1) / 2;
  std::atomic<int> wrong{0};
  Completion rounds;
  auto scheduler =
      Peer::Make(TaskSchedulerOptions{.workerCount = form.workers,
                                      .helpMarginBytes = form.pastMargin ? kAlwaysPastMargin : 0},
                 form.delays ? &delays : nullptr);
  // A few roots at once, so that waits, steals and sleeps interleave across trees.
  for (int round = 0; round < form.rounds; ++round) {
    Peer::Spawn(*scheduler, [&] {
      if (TreeSum(*scheduler, 0, kSize, 0) != kExpected) {
        wrong.fetch_add(1);
      }
      rounds.arrive();
    });
  }
  ASSERT_TRUE(rounds.waitFor(form.rounds)) << "a fork-join tree never completed";
  EXPECT_THAT(wrong.load(), Eq(0));
  EXPECT_TRUE(QuiescentAndCounted(*scheduler));
  scheduler.reset();
}

}  // namespace
}  // namespace donner::tasks
