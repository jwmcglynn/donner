#include "donner/base/tasks/TaskScope.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tasks/ParkerPool.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"
#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {

/// Reaches a scope's count and recorded parker.
class TaskScopeTestPeer {
public:
  static std::uint64_t Count(const TaskScope& scope) {
    return scope.count_.load(std::memory_order_relaxed);
  }

  static const Parker<>* RecordedParker(const TaskScope& scope) { return scope.link_.parker; }
};

namespace {

using Peer = TaskSchedulerTestPeer;
using ScopePeer = TaskScopeTestPeer;
using testing::Each;
using testing::Eq;
using testing::Ge;
using testing::Le;
using testing::NotNull;

/// The epoch of \p parker; the pointer stays valid while its pool lives.
std::uint64_t EpochOf(const Parker<>* parker) {
  return Parker<>::EpochOf(parker->word());
}

/// Counts destructions, so a closure's lifetime is visible after the task ran.
struct Tracker {
  std::atomic<int>* destroyed;
  ~Tracker() { destroyed->fetch_add(1); }
};

/// A closure that owns a \ref Tracker: running it counts a run, and destroying it, wherever that
/// happens, counts a destruction.
struct TrackedClosure {
  std::unique_ptr<Tracker> tracker;
  std::atomic<int>* runs;
  void operator()() const { runs->fetch_add(1); }
};

// With zero workers nothing is queued: spawns run inline, trySpawn declines, wait returns at once.
TEST(TaskScopeTest, ZeroWorkersRunSpawnsInlineAndWaitReturnsAtOnce) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
  std::vector<std::thread::id> ranOn;
  {
    TaskScope scope(scheduler);
    scope.spawn([&] { ranOn.push_back(std::this_thread::get_id()); });
    std::atomic<int> runs{0};
    std::atomic<int> destroyed{0};
    TrackedClosure closure{std::unique_ptr<Tracker>(new Tracker{&destroyed}), &runs};
    EXPECT_FALSE(scope.trySpawn(std::move(closure)));
    EXPECT_THAT(closure.tracker, NotNull()) << "a declined trySpawn leaves the closure";
    EXPECT_THAT(runs.load(), Eq(0));
    EXPECT_THAT(ScopePeer::Count(scope), Eq(0u));
    scope.wait();
  }
  EXPECT_THAT(ranOn, testing::ElementsAre(std::this_thread::get_id()));

  std::vector<char> order;
  Join(scheduler, [&] { order.push_back('a'); }, [&] { order.push_back('b'); });
  EXPECT_THAT(order, testing::ElementsAre('a', 'b')) << "with zero workers Join runs a then b";
}

class ScopeWorkerCounts : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(Workers, ScopeWorkerCounts, testing::Values(1, 4),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                           return "Workers" + std::to_string(info.param);
                         });

// Every task spawned by a non-worker runs exactly once, on a worker, and the wait sees what each
// wrote without any other synchronization.
TEST_P(ScopeWorkerCounts, ANonWorkerScopeRunsEveryTaskOnceOnAWorker) {
  constexpr int kTasks = 2000;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  std::vector<int> runs(kTasks, 0);
  std::vector<int> ranOnWorker(kTasks, -1);
  {
    TaskScope scope(scheduler);
    for (int i = 0; i < kTasks; ++i) {
      scope.spawn([&, i] {
        ++runs[static_cast<std::size_t>(i)];
        ranOnWorker[static_cast<std::size_t>(i)] = Peer::WorkerIndex(scheduler);
      });
    }
    scope.wait();
    EXPECT_THAT(ScopePeer::Count(scope), Eq(0u));
  }
  EXPECT_THAT(runs, Each(Eq(1)));
  EXPECT_THAT(ranOnWorker, Each(Ge(0))) << "a non-worker never runs its scope's tasks";
  EXPECT_THAT(Peer::ExternalLiveRecords(scheduler), Eq(0u));
}

// Tasks of the scope spawn into it from workers (flat spawns): the wait covers every level.
TEST_P(ScopeWorkerCounts, TasksOfTheScopeSpawnIntoIt) {
  constexpr int kRoots = 16;
  constexpr int kChildren = 16;
  constexpr int kGrandchildren = 4;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
  std::vector<int> leaves(kRoots * kChildren * kGrandchildren, 0);
  TaskScope scope(scheduler);
  for (int r = 0; r < kRoots; ++r) {
    scope.spawn([&, r] {
      for (int c = 0; c < kChildren; ++c) {
        scope.spawn([&, r, c] {
          for (int g = 0; g < kGrandchildren; ++g) {
            scope.spawn([&, r, c, g] {
              ++leaves[static_cast<std::size_t>((r * kChildren + c) * kGrandchildren + g)];
            });
          }
        });
      }
    });
  }
  scope.wait();
  EXPECT_THAT(leaves, Each(Eq(1)));
}

/// Spawns \p count tracked closures into \p scope and returns: the closures outlive this call.
void SpawnTracked(TaskScope& scope, int count, std::atomic<int>* runs,
                  std::atomic<int>* destroyed) {
  for (int i = 0; i < count; ++i) {
    scope.spawn(TrackedClosure{std::unique_ptr<Tracker>(new Tracker{destroyed}), runs});
  }
}

// A task outlives the statement that spawned it but not the scope: once the wait returns, every
// closure has run and been destroyed, and every record is back in its pool.
TEST(TaskScopeTest, ClosuresOutliveTheSpawningStatementButNotTheScope) {
  constexpr int kTasks = 200;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 2});
  std::atomic<int> runs{0};
  std::atomic<int> destroyed{0};
  {
    TaskScope scope(scheduler);
    SpawnTracked(scope, kTasks, &runs, &destroyed);
    scope.wait();
    EXPECT_THAT(runs.load(), Eq(kTasks));
    EXPECT_THAT(destroyed.load(), Eq(kTasks))
        << "a finished task's closure is destroyed before its "
           "decrement";
    EXPECT_THAT(Peer::ExternalLiveRecords(scheduler), Eq(0u))
        << "a finished task's record is returned before its decrement";
  }
}

// A spawn the scheduler cannot queue runs inline on the spawning thread and is not counted; a
// declined trySpawn leaves the closure with the caller.
TEST(TaskScopeTest, ADeclinedSpawnInAScopeRunsInlineAndIsNotCounted) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .maxExternalTasks = 1});
  Gate blocker;
  TaskScope scope(scheduler);
  scope.spawn([&] { blocker.arriveAndWait(); });
  ASSERT_TRUE(blocker.waitForArrival());
  EXPECT_THAT(ScopePeer::Count(scope), Eq(1u));

  std::thread::id ranOn;
  scope.spawn([&] { ranOn = std::this_thread::get_id(); });
  EXPECT_THAT(ranOn, Eq(std::this_thread::get_id())) << "at the external bound spawn runs inline";
  EXPECT_THAT(ScopePeer::Count(scope), Eq(1u)) << "an inline run is not counted";

  std::atomic<int> runs{0};
  std::atomic<int> destroyed{0};
  TrackedClosure closure{std::unique_ptr<Tracker>(new Tracker{&destroyed}), &runs};
  EXPECT_FALSE(scope.trySpawn(std::move(closure)));
  EXPECT_THAT(closure.tracker, NotNull()) << "a declined trySpawn leaves the closure";
  EXPECT_THAT(ScopePeer::Count(scope), Eq(1u));

  blocker.release();
  scope.wait();
  EXPECT_THAT(runs.load(), Eq(0));
  EXPECT_THAT(destroyed.load(), Eq(0));
}

// A flat spawn that finds its worker's deque full runs inline on that worker, uncounted, and the
// scope still completes.
TEST(TaskScopeTest, AFlatSpawnPastAFullDequeRunsInlineOnTheWorker) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .dequeCapacity = 1});
  int queuedRuns = 0;
  int inlineRuns = 0;
  bool inlineRanBeforeReturn = false;
  std::uint64_t countDuringInline = 0;
  TaskScope scope(scheduler);
  scope.spawn([&] {
    scope.spawn([&] { ++queuedRuns; });  // Fills the one-entry deque.
    bool ran = false;
    scope.spawn([&] {
      ran = true;
      ++inlineRuns;
      countDuringInline = ScopePeer::Count(scope);
    });
    inlineRanBeforeReturn = ran;
  });
  scope.wait();
  EXPECT_THAT(queuedRuns, Eq(1));
  EXPECT_THAT(inlineRuns, Eq(1));
  EXPECT_TRUE(inlineRanBeforeReturn) << "a spawn past a full deque runs inline";
  EXPECT_THAT(countDuringInline, Eq(2u)) << "the parent and the queued child, not the inline one";
}

// Spawns into one scope until trySpawn declines at the external bound: the scope is not complete
// until every one of those tasks ran.
TEST(TaskScopeTest, AScopeAtTheExternalBoundIsNotCompleteUntilEveryTaskRan) {
  constexpr std::size_t kBound = 8;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .maxExternalTasks = kBound});
  Gate blocker;
  std::atomic<std::size_t> runs{0};
  TaskScope scope(scheduler);
  scope.spawn([&] {
    blocker.arriveAndWait();
    runs.fetch_add(1);
  });
  ASSERT_TRUE(blocker.waitForArrival());
  std::size_t accepted = 1;
  while (scope.trySpawn([&] { runs.fetch_add(1); })) {
    ++accepted;
  }
  EXPECT_THAT(accepted, Eq(kBound));
  EXPECT_THAT(ScopePeer::Count(scope), Eq(kBound)) << "the count carries every queued task";
  EXPECT_THAT(runs.load(), Eq(0u));
  blocker.release();
  scope.wait();
  EXPECT_THAT(runs.load(), Eq(kBound));
}

// Join from a non-worker: a runs on the caller, b on a worker, and both finish before it returns.
TEST(TaskScopeTest, JoinFromANonWorkerRunsAOnTheCallerAndBOnAWorker) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 2});
  std::thread::id aRanOn;
  int bWorker = -1;
  int aResult = 0;
  int bResult = 0;
  Join(
      scheduler,
      [&] {
        aRanOn = std::this_thread::get_id();
        aResult = 1;
      },
      [&] {
        bWorker = Peer::WorkerIndex(scheduler);
        bResult = 2;
      });
  EXPECT_THAT(aRanOn, Eq(std::this_thread::get_id()));
  EXPECT_THAT(bWorker, Ge(0)) << "b goes to the global queue and runs on a worker";
  EXPECT_THAT(aResult + bResult, Eq(3));
  EXPECT_THAT(Peer::LiveScopes(scheduler), Eq(0u));
}

// Deterministic interleavings of a finisher's decrement and parker change with the creator's sleep.

/// A creator thread with a one-task scope whose task blocks until released. Everything it touches
/// is owned here and shared with the thread, so a thread detached after a failed wait touches
/// nothing out of scope.
struct CreatorThreadState {
  /// Starts the scheduler; holds must be added to \ref hooks before, while no worker reads them.
  void start(std::size_t workers) {
    scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = workers}, &hooks);
  }

  StepHooks hooks;
  Gate creatorHold;
  Gate task;
  std::unique_ptr<TaskScheduler> scheduler;
  std::atomic<const TaskScope*> scope{nullptr};
  Event spawned;
  Event returned;
};

/// Starts the creator thread: it builds a scope, spawns one task held at the state's task gate,
/// and waits. @return The thread; join it once `returned` is set, otherwise detach it.
std::thread StartCreator(const std::shared_ptr<CreatorThreadState>& state) {
  return std::thread([state] {
    {
      TaskScope scope(*state->scheduler);
      scope.spawn([state] { state->task.arriveAndWait(); });
      state->scope.store(&scope);
      state->spawned.set();
      scope.wait();
    }
    state->returned.set();
  });
}

/// Waits for a thread to arrive at \p gate. @return False, with \p thread detached, if it never
/// did.
bool ArrivedOrDetach(Gate& gate, std::thread& thread) {
  if (gate.waitForArrival()) {
    return true;
  }
  thread.detach();
  return false;
}

/// Joins \p thread if the creator returned under the hang guard, otherwise detaches it.
/// @return Whether it returned.
bool FinishCreator(std::thread& thread, const std::shared_ptr<CreatorThreadState>& state) {
  const bool returned = state->returned.wait();
  if (returned) {
    thread.join();
  } else {
    thread.detach();
  }
  return returned;
}

// The last task finishes, and changes the parker, before the creator sets its waiting bit: the
// creator's re-check after the bit sees the zero count.
TEST(TaskScopeInterleavingTest, AFinishBeforeTheCreatorsWaitingBitIsSeenByItsRecheck) {
  auto state = std::make_shared<CreatorThreadState>();
  state->hooks.holdAt(SchedulerStep::ScopeWaiterBeforePrepareWait, 1, &state->creatorHold);
  state->start(1);
  std::thread creator = StartCreator(state);
  ASSERT_TRUE(ArrivedOrDetach(state->creatorHold, creator));
  const TaskScope* scope = state->scope.load();
  const Parker<>* parker = ScopePeer::RecordedParker(*scope);
  const std::uint64_t epochBefore = EpochOf(parker);

  state->task.release();
  EXPECT_TRUE(PollUntil([&] { return EpochOf(parker) != epochBefore; }))
      << "the finisher must change the creator's parker";
  EXPECT_THAT(ScopePeer::Count(*scope), Eq(0u));

  state->creatorHold.release();
  EXPECT_TRUE(FinishCreator(creator, state))
      << "the re-check after the waiting bit must see the zero count";
  EXPECT_THAT(state->hooks.count(SchedulerStep::ScopeWaiterBeforeWait), Eq(0));
}

// The creator set its waiting bit and re-checked before the last task finished: the finisher's
// change must end its wait.
TEST(TaskScopeInterleavingTest, AFinishAfterTheCreatorsRecheckWakesIt) {
  auto state = std::make_shared<CreatorThreadState>();
  state->hooks.holdAt(SchedulerStep::ScopeWaiterBeforeWait, 1, &state->creatorHold);
  state->start(1);
  std::thread creator = StartCreator(state);
  ASSERT_TRUE(ArrivedOrDetach(state->creatorHold, creator));
  const Parker<>* parker = ScopePeer::RecordedParker(*state->scope.load());
  const std::uint64_t epochBefore = EpochOf(parker);
  EXPECT_THAT(parker->word() & Parker<>::kWaitingBit, Eq(Parker<>::kWaitingBit))
      << "the creator holds its waiting bit before it waits";

  state->task.release();
  EXPECT_TRUE(PollUntil([&] { return EpochOf(parker) != epochBefore; }))
      << "the finisher must change the creator's parker";

  state->creatorHold.release();
  EXPECT_TRUE(FinishCreator(creator, state)) << "the finisher's change must end the wait";
}

// The finisher changes the parker only after its decrement, and touches no part of the scope
// after it: the creator reads zero, returns and destroys the scope while the finisher is held
// between its decrement and its parker change. Under AddressSanitizer, a parker inside the scope
// would be a use after free here.
TEST(TaskScopeInterleavingTest, AFinisherTouchesNoScopeAfterItsDecrement) {
  StepHooks hooks;
  Gate finisherHold;
  hooks.holdAt(SchedulerStep::ScopeFinisherAfterDecrement, 1, &finisherHold);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);

  std::atomic<int> runs{0};
  std::atomic<int> destroyed{0};
  auto scope = std::make_unique<TaskScope>(*scheduler);
  const Parker<>* parker = ScopePeer::RecordedParker(*scope);
  const std::uint64_t epochBefore = EpochOf(parker);
  scope->spawn(TrackedClosure{std::unique_ptr<Tracker>(new Tracker{&destroyed}), &runs});
  ASSERT_TRUE(finisherHold.waitForArrival());

  EXPECT_THAT(ScopePeer::Count(*scope), Eq(0u));
  EXPECT_THAT(EpochOf(parker), Eq(epochBefore)) << "the parker changes only after the decrement";
  EXPECT_THAT(destroyed.load(), Eq(1)) << "the closure is destroyed before the decrement";
  EXPECT_THAT(Peer::ExternalLiveRecords(*scheduler), Eq(0u))
      << "the record is returned before the decrement";

  scope->wait();  // Reads zero and returns without sleeping.
  scope.reset();
  EXPECT_THAT(runs.load(), Eq(1));

  finisherHold.release();
  EXPECT_TRUE(PollUntil([&] { return EpochOf(parker) != epochBefore; }))
      << "the finisher changes the pooled parker after the scope is gone";
  scheduler.reset();
}

// A spawn counts its task before publishing it: the task runs and finishes while its publisher
// is still held between its push and its read of the awake count.
TEST(TaskScopeInterleavingTest, ATaskCountsBeforeItIsPublished) {
  auto state = std::make_shared<CreatorThreadState>();
  Gate busy;
  // The first publish is the busy task below; hold the second, the creator's.
  state->hooks.holdAt(SchedulerStep::AfterPublishPush, 2, &state->creatorHold);
  state->start(1);
  Peer::Spawn(*state->scheduler, [&] { busy.arriveAndWait(); });
  ASSERT_TRUE(busy.waitForArrival());

  std::thread creator([state] {
    {
      TaskScope scope(*state->scheduler);
      scope.spawn([state] { state->spawned.set(); });
      scope.wait();
    }
    state->returned.set();
  });
  ASSERT_TRUE(ArrivedOrDetach(state->creatorHold, creator));
  busy.release();  // The awake worker finishes and takes the creator's task from the global queue.
  EXPECT_TRUE(state->spawned.wait()) << "the queued task runs while its publisher is held";

  state->creatorHold.release();
  EXPECT_TRUE(FinishCreator(creator, state));
}

// The parker hand-out and return: a creator thread exits and its slot passes to the next thread
// that registers, while a finisher of the first thread's scope still holds the parker. The late
// change is a spurious wake for the new owner, which re-checks its own scope and sleeps again.
TEST(TaskScopeInterleavingTest, ALateChangeToAReusedParkerIsASpuriousWake) {
  auto state = std::make_shared<CreatorThreadState>();
  Gate finisherHold;
  state->hooks.holdAt(SchedulerStep::ScopeFinisherAfterDecrement, 1, &finisherHold);
  state->start(2);
  const std::shared_ptr<ParkerPool> pool = Peer::Pool(*state->scheduler);

  std::atomic<const Parker<>*> firstParker{nullptr};
  Event firstGo;
  std::thread first([state, &firstParker, &firstGo] {
    TaskScope scope(*state->scheduler);
    firstParker.store(ScopePeer::RecordedParker(scope));
    scope.spawn([] {});
    EXPECT_TRUE(firstGo.wait());
    scope.wait();
  });
  EXPECT_TRUE(finisherHold.waitForArrival());
  firstGo.set();
  first.join();
  EXPECT_THAT(pool->slotsInUse(), Eq(0u)) << "the exited thread returned its slot";

  std::thread second = StartCreator(state);  // Its task stays queued or held at its gate.
  if (!state->spawned.wait()) {
    second.detach();
    FAIL() << "the second creator never spawned";
  }
  const Parker<>* parker = ScopePeer::RecordedParker(*state->scope.load());
  EXPECT_THAT(parker, Eq(firstParker.load())) << "the slot passed to the next thread";
  EXPECT_TRUE(PollUntil([&] {
    return state->hooks.count(SchedulerStep::ScopeWaiterBeforePrepareWait) == 1 &&
           (parker->word() & Parker<>::kWaitingBit) != 0;
  })) << "the second creator sleeps on the reused parker";

  finisherHold.release();  // The first scope's finisher now changes the reused parker.
  EXPECT_TRUE(PollUntil([&] {
    return state->hooks.count(SchedulerStep::ScopeWaiterBeforePrepareWait) == 2;
  })) << "the spurious wake re-checks and sleeps again";
  EXPECT_THAT(ScopePeer::Count(*state->scope.load()), Eq(1u)) << "its own task is still held";

  state->task.release();
  EXPECT_TRUE(FinishCreator(second, state));
}

/// The worker counts the parker pool's liveness cases run at.
class PoolWorkerCounts : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(Workers, PoolWorkerCounts, testing::Values(1, 2, 4),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                           return "Workers" + std::to_string(info.param);
                         });

// A scheduler destroyed before a thread that holds one of its pooled parkers exits: the thread's
// entry keeps the pool alive, and the thread's exit returns the slot and frees the pool.
TEST_P(PoolWorkerCounts, ASchedulerDestroyedBeforeAThreadHoldingItsParkerExits) {
  const std::size_t poolsBefore = ParkerPool::LiveCountForTesting();
  std::size_t poolsAfterSchedulerGone = 0;
  std::thread thread([&] {
    auto scheduler =
        std::make_unique<TaskScheduler>(TaskSchedulerOptions{.workerCount = GetParam()});
    {
      TaskScope scope(*scheduler);
      scope.spawn([] {});
    }
    scheduler.reset();
    poolsAfterSchedulerGone = ParkerPool::LiveCountForTesting();
  });
  thread.join();
  EXPECT_THAT(poolsAfterSchedulerGone, Eq(poolsBefore + 1)) << "the thread's entry holds the pool";
  EXPECT_THAT(ParkerPool::LiveCountForTesting(), Eq(poolsBefore)) << "thread exit frees it";
}

// One non-worker creating, using and destroying schedulers in turn: each registration sweeps the
// pools of destroyed schedulers, so the thread never holds more than the current one.
TEST_P(PoolWorkerCounts, OneThreadUsingManySchedulersInTurnHoldsOnlyTheCurrentPool) {
  constexpr int kSchedulers = 1000;
  std::vector<std::size_t> poolsAfterRegistration;
  std::size_t poolsAtEnd = 0;
  std::size_t poolsBefore = 0;
  std::thread thread([&] {
    poolsBefore = ParkerPool::LiveCountForTesting();
    for (int i = 0; i < kSchedulers; ++i) {
      TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = GetParam()});
      TaskScope scope(scheduler);
      poolsAfterRegistration.push_back(ParkerPool::LiveCountForTesting());
      scope.spawn([] {});
    }
    TaskScheduler fresh(TaskSchedulerOptions{.workerCount = GetParam()});
    TaskScope scope(fresh);
    poolsAtEnd = ParkerPool::LiveCountForTesting();
  });
  thread.join();
  EXPECT_THAT(poolsAfterRegistration, Each(Le(poolsBefore + 2)));
  EXPECT_THAT(poolsAtEnd, Eq(poolsBefore + 1)) << "only the fresh scheduler's pool is live";
}

// Assertions on misuse.
TEST(TaskScopeDeathTest, AScopeCreatedOnAWorkerFails) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        Event done;
        Peer::Spawn(scheduler, [&] {
          TaskScope scope(scheduler);
          done.set();
        });
        (void)done.wait();
      },
      "TaskScope created on a worker");
}

TEST(TaskScopeDeathTest, AWaitFromAnotherThreadFails) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        TaskScope scope(scheduler);
        std::thread other([&] { scope.wait(); });
        other.join();
      },
      "joined on the thread that created it");
}

TEST(TaskScopeDeathTest, ASchedulerDestroyedWithALiveScopeFails) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        auto scheduler = std::make_unique<TaskScheduler>(TaskSchedulerOptions{.workerCount = 1});
        TaskScope scope(*scheduler);
        scheduler.reset();
      },
      "destroyed with a live TaskScope");
}

// Stress: scopes created, waited on and destroyed in a loop, so that every wait races its last
// task's finish; with no hooks and with short random delays at every protocol step.

struct ScopeStressForm {
  std::size_t workers;
  bool delays;
  int rounds;
};

std::ostream& operator<<(std::ostream& os, const ScopeStressForm& form) {
  return os << form.workers << " workers" << (form.delays ? ", injected delays" : "");
}

class TaskScopeStressTest : public testing::TestWithParam<ScopeStressForm> {
protected:
  static SchedulerTestHooks* hooks() {
    static DelayHooks delays;
    return GetParam().delays ? &delays : nullptr;
  }
};

INSTANTIATE_TEST_SUITE_P(Forms, TaskScopeStressTest,
                         testing::Values(ScopeStressForm{1, false, 2000},
                                         ScopeStressForm{4, false, 2000},
                                         ScopeStressForm{1, true, 500},
                                         ScopeStressForm{4, true, 500}),
                         [](const testing::TestParamInfo<ScopeStressForm>& info) {
                           return "Workers" + std::to_string(info.param.workers) +
                                  (info.param.delays ? "InjectedDelay" : "NoHooks");
                         });

/// Runs \p body on a thread under the hang guard. @return False if it did not finish; the thread
/// is then detached with \p state, which it keeps alive.
template <typename State, typename Body>
bool RunGuarded(const std::shared_ptr<State>& state, Body body) {
  auto done = std::make_shared<Event>();
  std::thread thread([state, done, body] {
    body(*state);
    done->set();
  });
  const bool finished = done->wait();
  if (finished) {
    thread.join();
  } else {
    thread.detach();
  }
  return finished;
}

// One task per scope, so every wait races the task's decrement and parker change.
TEST_P(TaskScopeStressTest, EveryWaitReturnsOnceItsOnlyTaskFinished) {
  struct State {
    std::unique_ptr<TaskScheduler> scheduler;
    std::vector<int> results;
  };
  auto state = std::make_shared<State>();
  state->scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = GetParam().workers}, hooks());
  state->results.assign(static_cast<std::size_t>(GetParam().rounds), 0);
  const bool finished = RunGuarded(state, [](State& s) {
    for (std::size_t i = 0; i < s.results.size(); ++i) {
      TaskScope scope(*s.scheduler);
      scope.spawn([&s, i] { s.results[i] = static_cast<int>(i) + 1; });
      scope.wait();
      EXPECT_THAT(s.results[i], Eq(static_cast<int>(i) + 1)) << "round " << i;
    }
  });
  ASSERT_TRUE(finished) << "a creator's wait never returned";
}

// Several non-workers at once, each with scopes whose tasks spawn into them from workers.
TEST_P(TaskScopeStressTest, ConcurrentNonWorkerScopesWithFlatSpawnsComplete) {
  constexpr int kCreators = 3;
  constexpr int kChildren = 4;
  struct State {
    std::unique_ptr<TaskScheduler> scheduler;
    std::atomic<int> failures{0};
  };
  auto state = std::make_shared<State>();
  state->scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = GetParam().workers}, hooks());
  const int rounds = GetParam().rounds / 10;
  const bool finished = RunGuarded(state, [rounds](State& s) {
    std::vector<std::thread> creators;
    for (int c = 0; c < kCreators; ++c) {
      creators.emplace_back([&s, rounds] {
        for (int round = 0; round < rounds; ++round) {
          std::vector<int> leaves(kChildren, 0);
          {
            TaskScope scope(*s.scheduler);
            scope.spawn([&] {
              for (int k = 0; k < kChildren; ++k) {
                scope.spawn([&leaves, k] { ++leaves[static_cast<std::size_t>(k)]; });
              }
            });
          }
          for (const int leaf : leaves) {
            if (leaf != 1) {
              s.failures.fetch_add(1);
            }
          }
        }
      });
    }
    for (std::thread& creator : creators) {
      creator.join();
    }
  });
  ASSERT_TRUE(finished) << "a creator's wait never returned";
  EXPECT_THAT(state->failures.load(), Eq(0)) << "a leaf ran other than once before its wait ended";
}

}  // namespace
}  // namespace donner::tasks
