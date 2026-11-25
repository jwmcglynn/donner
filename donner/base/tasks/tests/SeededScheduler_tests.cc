#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tasks/AsyncTaskScope.h"
#include "donner/base/tasks/Task.h"
#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/CoroutineTestUtils.h"

namespace donner::tasks {
namespace {

using testing::ElementsAre;
using testing::Eq;
using testing::IsEmpty;
using testing::Ne;

TEST(SeededSchedulerTest, PositionalOptionsPreserveTheWorkerAndDequeConfiguration) {
  const TaskSchedulerOptions options{4, 64};
  EXPECT_THAT(options.workerCount, Eq(4u));
  EXPECT_THAT(options.dequeCapacity, Eq(64u));
  EXPECT_FALSE(options.seed.has_value());
}

std::vector<int> Replay(std::uint64_t seed, std::size_t requestedWorkers) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = requestedWorkers, .seed = seed});
  EXPECT_THAT(scheduler.workerCount(), Eq(0u));
  std::vector<int> trace;
  const auto owner = std::this_thread::get_id();
  TaskScope scope(scheduler);
  for (int i = 0; i < 24; ++i) {
    scope.spawn([&, i] {
      EXPECT_THAT(std::this_thread::get_id(), Eq(owner));
      trace.push_back(i);
    });
  }
  EXPECT_THAT(trace, IsEmpty());
  scope.wait();
  return trace;
}

TEST(SeededSchedulerTest, ReplaysChoicesAcrossRequestedWorkerCountsAndUsesTheWholeSeed) {
  const auto trace = Replay(0, 0);
  EXPECT_THAT(Replay(0, 4), Eq(trace));
  EXPECT_THAT(Replay(std::uint64_t{1} << 32, 4), Ne(trace));
  auto sorted = trace;
  std::sort(sorted.begin(), sorted.end());
  std::vector<int> expected(24);
  std::iota(expected.begin(), expected.end(), 0);
  EXPECT_THAT(sorted, Eq(expected));
}

TEST(SeededSchedulerTest, JoinQueuesTheSecondBranchEvenWithNoPhysicalWorkers) {
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = 3, .maxTasksPerCreator = 1, .seed = 7});
  EXPECT_THAT(scheduler.workerCount(), Eq(0u));
  EXPECT_FALSE(scheduler.threadingDisabledByConfiguration());
  EXPECT_THAT(scheduler.schedulingWidth(), Eq(4u));
  std::vector<int> order;
  Join(
      scheduler,
      [&] {
        order.push_back(1);
        TaskScope probe(scheduler);
        // The deferred second branch occupies the replay pool's only live record.
        EXPECT_FALSE(probe.trySpawn([] {}));
      },
      [&] { order.push_back(2); });
  EXPECT_THAT(order, ElementsAre(1, 2));
  TaskScope after(scheduler);
  EXPECT_TRUE(after.trySpawn([] {}));
}

TEST(SeededSchedulerTest, ARunningRecordCountsAtTheBoundAndDeclinesKeepTheCallable) {
  TaskScheduler scheduler(TaskSchedulerOptions{.maxTasksPerCreator = 1, .seed = 41});
  TaskScope scope(scheduler);
  std::vector<int> trace;
  EXPECT_TRUE(scope.trySpawn([&] {
    auto work = [&, value = std::make_unique<int>(1)] {
      EXPECT_NE(value, nullptr);
      if (value) {
        trace.push_back(*value);
      }
    };
    const bool admitted = scope.trySpawn(std::move(work));
    EXPECT_FALSE(admitted);
    if (!admitted) {
      work();
    }
    scope.spawn([&] { trace.push_back(2); });
    trace.push_back(3);
  }));
  EXPECT_FALSE(scope.trySpawn([&] { trace.push_back(-1); }));
  EXPECT_THAT(trace, IsEmpty());
  scope.wait();
  EXPECT_THAT(trace, ElementsAre(1, 2, 3));
  EXPECT_TRUE(scope.trySpawn([&] { trace.push_back(4); }));
  scope.wait();
  EXPECT_THAT(trace, ElementsAre(1, 2, 3, 4));
}

TEST(SeededSchedulerTest, NestedJoinsFinishDescendantsAndStoppedScopesDiscardQueuedWork) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 4, .seed = 19});
  std::array<int, 8> results{};
  int cancelledCalls = 0;
  TaskScope roots(scheduler);
  for (std::size_t i = 0; i < results.size(); ++i) {
    roots.spawn([&, i] {
      int left = 0;
      int right = 0;
      Join(
          scheduler,
          [&] {
            TaskScope children(scheduler);
            children.spawn([&] { children.spawn([&] { left = 7; }); });
          },
          [&] { right = 5; });
      results[i] = left + right;
      TaskScope cancelled(scheduler);
      cancelled.spawn([&] { ++cancelledCalls; });
      cancelled.requestStop();
    });
  }
  roots.wait();
  EXPECT_THAT(results, testing::Each(Eq(12)));
  EXPECT_THAT(cancelledCalls, Eq(0));
}

TEST(SeededSchedulerTest, FlatSiblingBacklogDoesNotNestItsBodiesDuringAnInnerJoin) {
  TaskScheduler scheduler(TaskSchedulerOptions{.seed = 3});
  constexpr int kSiblings = 512;
  int active = 0;
  int maximumActive = 0;
  int completed = 0;
  TaskScope siblings(scheduler);
  for (int i = 0; i < kSiblings; ++i) {
    siblings.spawn([&] {
      ++active;
      maximumActive = std::max(maximumActive, active);
      Join(scheduler, [] {}, [&] { ++completed; });
      --active;
    });
  }
  siblings.wait();
  EXPECT_THAT(maximumActive, Eq(1));
  EXPECT_THAT(completed, Eq(kSiblings));
}

Task<int> JoinOnCaller(TaskScheduler& scheduler, const std::thread::id& owner) {
  co_await scheduler.schedule();
  EXPECT_THAT(std::this_thread::get_id(), Eq(owner));
  int result = 0;
  AsyncTaskScope scope(scheduler);
  scope.spawn([&] {
    scope.spawn([&] { result += 13; });
    result += 5;
  });
  scope.spawn([&] { result += 7; });
  EXPECT_THAT(result, Eq(0));
  co_await scope.join();
  EXPECT_THAT(std::this_thread::get_id(), Eq(owner));
  co_return result;
}

CoroutineTestRoot Collect(Task<int> task, int& result) {
  result = co_await task;
}

TEST(SeededSchedulerTest, ScheduleIsReadyAndAsyncJoinRunsOutstandingWorkOnTheCaller) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 4, .seed = 0});
  EXPECT_TRUE(scheduler.schedule().await_ready());
  const auto owner = std::this_thread::get_id();
  int result = 0;
  Event done;
  auto root = Collect(JoinOnCaller(scheduler, owner), result);
  root.start(done);
  EXPECT_THAT(result, Eq(25));
  EXPECT_TRUE(done.wait());
}

TEST(SeededSchedulerDeathTest, RejectsUseFromAnotherThread) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.seed = 0});
        std::thread other([&] { TaskScope scope(scheduler); });
        other.join();
      },
      "only on its constructing thread");
}

}  // namespace
}  // namespace donner::tasks
