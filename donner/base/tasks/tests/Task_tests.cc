#include "donner/base/tasks/Task.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tasks/tests/CoroutineTestUtils.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::Eq;
using testing::Gt;
using testing::Le;
using testing::Not;

Task<std::unique_ptr<int>> Value(TaskScheduler& scheduler, int& calls) {
  ++calls;
  co_await scheduler.schedule();
  co_return std::make_unique<int>(9);
}

Task<int> Nested(TaskScheduler& scheduler, int& calls, int depth) {
  if (depth == 0) {
    co_return *(co_await Value(scheduler, calls));
  }
  co_return 1 + co_await Nested(scheduler, calls, depth - 1);
}

CoroutineTestRoot Observe(Task<int>& task, int& result, std::thread::id& where) {
  result = co_await task;
  where = std::this_thread::get_id();
}

TEST(TaskTest, NestedResultsReachTheAwaiterOnTheScheduledThread) {
  for (std::size_t workers : {0, 1, 4}) {
    SCOPED_TRACE(workers);
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = workers});
    int calls = 0;
    auto task = Nested(scheduler, calls, 64);
    auto owner = std::move(task);
    EXPECT_THAT(calls, Eq(0));
    int result = 0;
    std::thread::id where;
    Event done;
    auto root = Observe(owner, result, where);
    root.start(done);
    ASSERT_THAT(done.wait(), Eq(true));
    EXPECT_THAT(result, Eq(73));
    EXPECT_THAT(calls, Eq(1));
    if (workers == 0) {
      EXPECT_THAT(where, Eq(std::this_thread::get_id()));
    } else {
      EXPECT_THAT(where, Not(Eq(std::this_thread::get_id())));
    }
  }
}

struct MemberFactory {
  Task<int> value(TaskScheduler& scheduler, int value) const {
    co_await scheduler.schedule();
    co_return value;
  }
};

TEST(TaskTest, MemberCoroutinesUseTheFirstExplicitSchedulerParameter) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
  const MemberFactory factory;
  auto task = factory.value(scheduler, 15);
  Event done;
  int result = 0;
  std::thread::id where;
  auto root = Observe(task, result, where);
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(result, Eq(15));
}

TEST(TaskTest, FreesAnUnstartedTaskWithoutRunningItsBody) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
  int calls = 0;
  {
    auto task = Nested(scheduler, calls, 0);
    EXPECT_THAT(calls, Eq(0));
  }
  EXPECT_THAT(calls, Eq(0));
}

TEST(TaskTest, BoundsOnlyFreeFrameBytesAndReusesExactSizes) {
  constexpr std::size_t kBytes = 129;
  for (std::size_t limit : {0u, 512u}) {
    SCOPED_TRACE(limit);
    TaskScheduler scheduler(
        TaskSchedulerOptions{.workerCount = 0, .retainedCoroutineFrameBytes = limit});
    std::array<void*, 8> frames;
    for (void*& frame : frames) {
      frame = Peer::AllocateCoroutineFrame(scheduler, kBytes);
    }
    EXPECT_THAT(Peer::RetainedCoroutineFrameBytes(scheduler), Eq(0u));
    for (void* frame : frames) {
      CoroutineFramePool::Release(frame, kBytes);
    }

    const std::size_t retained = Peer::RetainedCoroutineFrameBytes(scheduler);
    EXPECT_THAT(retained, Le(limit));
    if (limit == 0) {
      EXPECT_THAT(retained, Eq(0u));
    } else {
      EXPECT_THAT(retained, Gt(0u));
      void* const frame = Peer::AllocateCoroutineFrame(scheduler, kBytes);
      EXPECT_THAT(Peer::RetainedCoroutineFrameBytes(scheduler), testing::Lt(retained));
      CoroutineFramePool::Release(frame, kBytes);
      EXPECT_THAT(Peer::RetainedCoroutineFrameBytes(scheduler), Eq(retained));
    }
  }
}

TEST(TaskTest, AllocationsAreAlignedAndReturnToTheAllocatingScheduler) {
  TaskScheduler allocating(TaskSchedulerOptions{.workerCount = 0});
  TaskScheduler other(TaskSchedulerOptions{.workerCount = 0});
  constexpr std::size_t kBytes = 129;
  void* const frame = Peer::AllocateCoroutineFrame(allocating, kBytes);
  EXPECT_THAT(reinterpret_cast<std::uintptr_t>(frame) % 128, Eq(0u));
  std::thread otherThread([&] { CoroutineFramePool::Release(frame, kBytes); });
  otherThread.join();
  EXPECT_THAT(Peer::RetainedCoroutineFrameBytes(allocating), Gt(0u));
  EXPECT_THAT(Peer::RetainedCoroutineFrameBytes(other), Eq(0u));
  void* const reused = Peer::AllocateCoroutineFrame(allocating, kBytes);
  EXPECT_THAT(reused, Eq(frame));
  CoroutineFramePool::Release(reused, kBytes);
}

struct alignas(128) AlignedValue {
  int value = 31;
};

// The address escapes on both sides of suspension so the local occupies the frame.
Task<AlignedValue> AlignedResult(TaskScheduler& scheduler, const void*& before,
                                 std::size_t& offsetAfter) {
  AlignedValue local;
  before = &local;
  co_await scheduler.schedule();
  offsetAfter = reinterpret_cast<std::uintptr_t>(&local) % alignof(AlignedValue);
  co_return local;
}

CoroutineTestRoot ObserveAlignment(Task<AlignedValue>& task, int& result) {
  result = (co_await task).value;
}

TEST(TaskTest, PreservesOverAlignmentOfThePromiseAndASuspendedLocal) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  const void* before = nullptr;
  std::size_t offsetAfter = 1;
  auto task = AlignedResult(scheduler, before, offsetAfter);
  int result = 0;
  Event done;
  auto root = ObserveAlignment(task, result);
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(result, Eq(31));
  EXPECT_THAT(before, testing::NotNull());
  EXPECT_THAT(reinterpret_cast<std::uintptr_t>(before) % alignof(AlignedValue), Eq(0u));
  EXPECT_THAT(offsetAfter, Eq(0u));
}

Task<void> FillDequeAndHop(TaskScheduler& scheduler, int& fills, int& completed) {
  co_await scheduler.schedule();
  Peer::Spawn(scheduler, [&] { ++fills; });
  Peer::Spawn(scheduler, [&] { ++fills; });
  co_await scheduler.schedule();
  completed = Peer::WorkerIndex(scheduler);
}

CoroutineTestRoot FinishOnWorker(Task<void> task, std::thread::id& thread) {
  {
    auto owner = std::move(task);
    co_await owner;
  }
  thread = std::this_thread::get_id();
}

TEST(TaskTest, FullWorkerDequeFallsBackToGlobalAndOwnerIsDestroyedOnTheWorker) {
  StepHooks hooks;
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1, .dequeCapacity = 2}, &hooks);
  int fills = 0;
  int completed = -1;
  std::thread::id thread;
  Event done;
  auto root = FinishOnWorker(FillDequeAndHop(*scheduler, fills, completed), thread);
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(completed, Eq(0));
  EXPECT_THAT(fills, Eq(2));
  EXPECT_THAT(thread, Not(Eq(std::this_thread::get_id())));
  EXPECT_THAT(hooks.count(SchedulerStep::ResumptionPublished), Eq(2));
}

CoroutineTestRoot HopAndRelease(TaskScheduler& scheduler, int& worker) {
  co_await scheduler.schedule();
  worker = Peer::WorkerIndex(scheduler);
}

TEST(TaskTest, OutsideScheduleWakesAWorkerAfterItsSleepRecheck) {
  Gate sleeper;
  HoldOnceHooks hooks(SchedulerStep::BeforeSleeperWaits, &sleeper);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  const bool held = sleeper.waitForArrival();
  int worker = -1;
  Event done;
  auto root = HopAndRelease(*scheduler, worker);
  root.start(done);
  const bool wokeWorker = Peer::State(*scheduler, 0) == WorkerState::Awake;
  if (!wokeWorker) {
    // Allow the root to finish even if the resumption failed to wake the worker.
    Peer::Spawn(*scheduler, [] {});
  }
  sleeper.release();
  EXPECT_THAT(held, Eq(true));
  EXPECT_THAT(wokeWorker, Eq(true));
  ASSERT_THAT(done.wait(), Eq(true));
  EXPECT_THAT(worker, Eq(0));
  EXPECT_THAT(sleeper.heldPastHangGuard(), Eq(false));
}

TEST(TaskTest, ResumesWhileTheNonWorkerPublisherIsStillInAwaitSuspend) {
  Gate taskGate;
  Gate publishGate;
  StepHooks hooks;
  hooks.holdAt(SchedulerStep::ResumptionPublished, 1, &publishGate);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  Peer::Spawn(*scheduler, [&] { taskGate.arriveAndWait(); });
  int worker = -1;
  Event done;
  auto root = HopAndRelease(*scheduler, worker);
  std::thread publisher([&] { root.start(done); });
  const bool published = publishGate.waitForArrival();
  taskGate.release();
  const bool finished = done.wait();
  if (finished) {
    scheduler.reset();
  }
  publishGate.release();
  publisher.join();
  EXPECT_THAT(published, Eq(true));
  EXPECT_THAT(finished, Eq(true));
  EXPECT_THAT(worker, Eq(0));
  EXPECT_THAT(publishGate.heldPastHangGuard(), Eq(false));
}

Task<void> Suspend(TaskScheduler& scheduler) {
  (void)scheduler;
  co_await std::suspend_always{};
}

Task<void> Complete(TaskScheduler& scheduler) {
  co_await scheduler.schedule();
}

CoroutineTestRoot WaitForTask(Task<void>& task) {
  co_await task;
}

struct ContinuationStackDepth {
  std::size_t samples = 0;
  std::size_t least = std::numeric_limits<std::size_t>::max();
  std::size_t most = 0;

  void sample(std::size_t remaining) {
    ++samples;
    least = std::min(least, remaining);
    most = std::max(most, remaining);
  }
};

Task<void> ObserveContinuationStack(TaskScheduler& scheduler, Task<void>* child,
                                    ContinuationStackDepth& depth) {
  if (child != nullptr) {
    co_await *child;
  } else {
    co_await scheduler.schedule();
  }
  // Sample the same code path at the leaf and as each parent resumes on the worker.
  if (const auto remaining = Peer::StackRemaining()) {
    depth.sample(*remaining);
  }
}

TEST(TaskTest, SymmetricTransferKeepsWorkerStackDepthBounded) {
  constexpr std::size_t kLinks = 64;
  constexpr std::size_t kStackVariation = 256;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  ContinuationStackDepth depth;
  // Keep each owner alive until the worker has returned from the entire chain.
  std::vector<Task<void>> chain;
  chain.reserve(kLinks + 1);
  chain.push_back(ObserveContinuationStack(scheduler, nullptr, depth));
  for (std::size_t i = 0; i < kLinks; ++i) {
    chain.push_back(ObserveContinuationStack(scheduler, &chain.back(), depth));
  }
  Event done;
  auto root = WaitForTask(chain.back());
  root.start(done);
  ASSERT_THAT(done.wait(), Eq(true));
  Event unwound;
  Peer::Spawn(scheduler, [&] { unwound.set(); });
  ASSERT_THAT(unwound.wait(), Eq(true));
  ASSERT_THAT(depth.samples, Eq(kLinks + 1));
  const std::size_t span = depth.most - depth.least;
  RecordProperty("stack_span_bytes", std::to_string(span));
  EXPECT_THAT(span, Le(kStackVariation));
}

TEST(TaskDeathTest, RejectsDestroyingAnOwnerBeforeItsAwaitCompletes) {
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
        auto task = Suspend(scheduler);
        auto root = WaitForTask(task);
        Event done;
        root.start(done);
        task = Complete(scheduler);
      },
      "A Task owner was destroyed before its await completed");
}

TEST(TaskDeathTest, RejectsDestroyingTheSchedulerWithASuspendedFrame) {
  EXPECT_DEATH(
      {
        auto scheduler = std::make_unique<TaskScheduler>(TaskSchedulerOptions{.workerCount = 0});
        auto task = Suspend(*scheduler);
        auto root = WaitForTask(task);
        Event done;
        root.start(done);
        scheduler.reset();
      },
      "destroyed with live coroutine frames");
}

TEST(TaskDeathTest, RejectsDestroyingTheSchedulerWithAQueuedScheduleAwait) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        auto scheduler = std::make_unique<TaskScheduler>(TaskSchedulerOptions{.workerCount = 1});
        Gate running;
        Peer::Spawn(*scheduler, [&] { running.arriveAndWait(); });
        auto task = Complete(*scheduler);
        auto root = WaitForTask(task);
        Event done;
        root.start(done);
        scheduler.reset();
        running.release();
      },
      "destroyed with live coroutine frames");
}

CoroutineTestRoot WaitTwice(Task<void>& task) {
  co_await task;
  co_await task;
}

TEST(TaskDeathTest, RejectsASecondAwait) {
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
        auto task = Complete(scheduler);
        auto root = WaitTwice(task);
        Event done;
        root.start(done);
      },
      "A Task may be awaited only once");
}

}  // namespace
}  // namespace donner::tasks
