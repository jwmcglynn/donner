#include "donner/base/tasks/AsyncTaskScope.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

#include "donner/base/tasks/Task.h"
#include "donner/base/tasks/tests/CoroutineTestUtils.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::Each;
using testing::Eq;

CoroutineTestRoot WaitForTask(Task<void> task) {
  co_await task;
}

Task<void> JoinTasks(TaskScheduler& scheduler, std::array<int, 8>& results, int& total) {
  AsyncTaskScope scope(scheduler);
  for (std::size_t i = 0; i < results.size(); ++i) {
    scope.spawn([&, i] { results[i] = 7; });
  }
  co_await scope.join();
  for (int result : results) {
    total += result;
  }
}

TEST(AsyncTaskScopeTest, JoinsAndReadsPlainResultsAtEveryWorkerCount) {
  for (std::size_t workers : {0, 1, 4}) {
    SCOPED_TRACE(workers);
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = workers});
    std::array<int, 8> results{};
    int total = 0;
    Event done;
    auto root = WaitForTask(JoinTasks(scheduler, results, total));
    root.start(done);
    ASSERT_THAT(done.wait(), Eq(true));
    EXPECT_THAT(total, Eq(56));
    EXPECT_THAT(results, Each(Eq(7)));
    EXPECT_THAT(Peer::LiveScopes(scheduler), Eq(0u));
  }
}

Task<void> SpawnDescendant(TaskScheduler& scheduler, TaskScheduler& scopeScheduler, Gate& work,
                           int& checked) {
  (void)scheduler;
  AsyncTaskScope scope(scopeScheduler);
  int payload = 0;
  scope.spawn([&] {
    work.arriveAndWait();
    scope.spawn([&] { payload = 17; });
  });
  co_await scope.join();
  checked = payload;
}

TEST(AsyncTaskScopeTest, FlatSpawnFromOutstandingWorkRemainsOpenAfterJoinRegisters) {
  Gate work;
  Gate resume;
  StepHooks hooks;
  hooks.holdAt(SchedulerStep::AsyncJoinBeforeEnqueue, 1, &resume);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  int checked = 0;
  Event done;
  auto root = WaitForTask(SpawnDescendant(*scheduler, *scheduler, work, checked));
  root.start(done);
  EXPECT_THAT(hooks.count(SchedulerStep::AsyncJoinRegistered), Eq(1));
  work.release();
  const bool aboutToEnqueue = resume.waitForArrival();
  resume.release();
  EXPECT_THAT(aboutToEnqueue, Eq(true));
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(checked, Eq(17));
  EXPECT_THAT(Peer::LiveScopes(*scheduler), Eq(0u));
  EXPECT_THAT(hooks.count(SchedulerStep::AfterPublishPush), Eq(1));
}

/// Uses relaxed flags deliberately: the only synchronizing read of the worker's payload must
/// be the creator's failed join-registration CAS.
class FinishBeforeRegistration : public SchedulerTestHooks {
public:
  void at(SchedulerStep step, std::size_t /*worker*/) override {
    if (step == SchedulerStep::AsyncJoinBeforeRegister) {
      workerMayFinish.store(true, std::memory_order_relaxed);
      observedFinish = PollUntil([&] { return finished.load(std::memory_order_relaxed); });
    } else if (step == SchedulerStep::AsyncJoinFinisherAfterDecrement) {
      finished.store(true, std::memory_order_relaxed);
    } else if (step == SchedulerStep::AsyncJoinRegistered) {
      registered.store(true, std::memory_order_relaxed);
    }
  }

  std::atomic<bool> workerMayFinish{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> registered{false};
  bool observedFinish = false;
  bool observedRegistrationAttempt = false;
};

Task<void> JoinAfterCompletionWins(TaskScheduler& scheduler, FinishBeforeRegistration& hooks,
                                   int& checked) {
  AsyncTaskScope scope(scheduler);
  int payload = 0;
  scope.spawn([&] {
    hooks.observedRegistrationAttempt =
        PollUntil([&] { return hooks.workerMayFinish.load(std::memory_order_relaxed); });
    payload = 41;
  });
  co_await scope.join();
  checked = payload;
}

TEST(AsyncTaskScopeTest, CompletionWinsRegistrationCASAndPublishesPlainPayload) {
  FinishBeforeRegistration hooks;
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  int checked = 0;
  Event done;
  auto root = WaitForTask(JoinAfterCompletionWins(*scheduler, hooks, checked));
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(hooks.observedFinish, Eq(true));
  EXPECT_THAT(hooks.observedRegistrationAttempt, Eq(true));
  EXPECT_THAT(hooks.registered.load(std::memory_order_relaxed), Eq(false));
  EXPECT_THAT(checked, Eq(41));
}

/// Does not publish the continuation through any synchronization other than the join flag.
class RegisterBeforeCompletion : public SchedulerTestHooks {
public:
  void at(SchedulerStep step, std::size_t /*worker*/) override {
    if (step == SchedulerStep::AsyncJoinRegistered) {
      registered.store(true, std::memory_order_relaxed);
    }
  }

  std::atomic<bool> registered{false};
};

Task<void> JoinBeforeCompletionWins(TaskScheduler& scheduler, RegisterBeforeCompletion& hooks,
                                    int& checked, bool& observedRegistration) {
  AsyncTaskScope scope(scheduler);
  int payload = 0;
  scope.spawn([&] {
    observedRegistration =
        PollUntil([&] { return hooks.registered.load(std::memory_order_relaxed); });
    payload = 53;
  });
  co_await scope.join();
  checked = payload;
}

TEST(AsyncTaskScopeTest, RegistrationWinsCASAndPublishesTheContinuation) {
  RegisterBeforeCompletion hooks;
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  int checked = 0;
  bool observedRegistration = false;
  Event done;
  auto root =
      WaitForTask(JoinBeforeCompletionWins(*scheduler, hooks, checked, observedRegistration));
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(observedRegistration, Eq(true));
  EXPECT_THAT(checked, Eq(53));
}

Task<void> JoinAlreadyComplete(TaskScheduler& scheduler, FinishBeforeRegistration& hooks,
                               int& checked) {
  AsyncTaskScope scope(scheduler);
  int payload = 0;
  scope.spawn([&] { payload = 29; });
  hooks.observedFinish = PollUntil([&] { return hooks.finished.load(std::memory_order_relaxed); });
  co_await scope.join();
  checked = payload;
}

TEST(AsyncTaskScopeTest, InitialZeroLoadPublishesPlainPayloadWithoutRegistering) {
  FinishBeforeRegistration hooks;
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  int checked = 0;
  Event done;
  auto root = WaitForTask(JoinAlreadyComplete(*scheduler, hooks, checked));
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(hooks.observedFinish, Eq(true));
  EXPECT_THAT(hooks.workerMayFinish.load(std::memory_order_relaxed), Eq(false));
  EXPECT_THAT(hooks.registered.load(std::memory_order_relaxed), Eq(false));
  EXPECT_THAT(checked, Eq(29));
}

TEST(AsyncTaskScopeTest, WorkerCanResumeAndDestroyScopeWhileAwaitSuspendIsStillReturning) {
  Gate work;
  Gate registration;
  StepHooks hooks;
  hooks.holdAt(SchedulerStep::AsyncJoinRegistered, 1, &registration);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  int checked = 0;
  Event done;
  TaskScheduler frameScheduler(TaskSchedulerOptions{});
  auto root = WaitForTask(SpawnDescendant(frameScheduler, *scheduler, work, checked));
  std::thread creator([&] { root.start(done); });
  const bool registered = registration.waitForArrival();
  work.release();
  const bool finished = done.wait();
  const std::size_t scopesWhileHeld = Peer::LiveScopes(*scheduler);
  if (finished) {
    scheduler.reset();
  }
  registration.release();
  creator.join();
  EXPECT_THAT(registered, Eq(true));
  ASSERT_THAT(finished, Eq(true));
  EXPECT_THAT(scopesWhileHeld, Eq(0u));
  EXPECT_THAT(checked, Eq(17));
  EXPECT_THAT(registration.heldPastHangGuard(), Eq(false));
}

Task<void> EmptyAndInline(TaskScheduler& scheduler, int& called, bool& queued) {
  AsyncTaskScope scope(scheduler);
  queued = scope.trySpawn([&] { ++called; });
  scope.spawn([&] { ++called; });
  co_await scope.join();
}

TEST(AsyncTaskScopeTest, ZeroWorkersDeclineTrySpawnAndCompleteJoinInline) {
  StepHooks hooks;
  auto scheduler = Peer::Make(TaskSchedulerOptions{}, &hooks);
  Event done;
  int called = 0;
  bool queued = true;
  auto root = WaitForTask(EmptyAndInline(*scheduler, called, queued));
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(called, Eq(1));
  EXPECT_THAT(queued, Eq(false));
  EXPECT_THAT(hooks.count(SchedulerStep::AsyncJoinRegistered), Eq(0));
  EXPECT_THAT(hooks.count(SchedulerStep::AfterPublishPush), Eq(0));
}

TEST(AsyncTaskScopeDeathTest, RejectsDestroyingAnUnjoinedScope) {
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{});
        const AsyncTaskScope scope(scheduler);
      },
      "must be joined before destruction");
}

Task<void> JoinTwice(TaskScheduler& scheduler) {
  AsyncTaskScope scope(scheduler);
  co_await scope.join();
  co_await scope.join();
}

TEST(AsyncTaskScopeDeathTest, RejectsASecondJoin) {
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{});
        Event done;
        auto root = WaitForTask(JoinTwice(scheduler));
        root.start(done);
      },
      "may be joined only once");
}

Task<void> Reopen(TaskScheduler& scheduler) {
  AsyncTaskScope scope(scheduler);
  co_await scope.join();
  scope.spawn([] {});
}

TEST(AsyncTaskScopeDeathTest, RejectsReopeningAfterJoin) {
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{});
        Event done;
        auto root = WaitForTask(Reopen(scheduler));
        root.start(done);
      },
      "cannot be reopened after join");
}

}  // namespace
}  // namespace donner::tasks
