#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>

#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::Eq;

TEST(AdoptedThreadTest, NoSlotInInlineSeededOrDisabledConfigurations) {
  TaskScheduler inlineScheduler(TaskSchedulerOptions{.workerCount = 0});
  TaskScheduler replay(TaskSchedulerOptions{.workerCount = 2, .seed = 0});
  TaskScheduler noSlots(TaskSchedulerOptions{.workerCount = 1, .adoptableThreads = 0});
  EXPECT_FALSE(inlineScheduler.adopt());
  EXPECT_FALSE(replay.adopt());
  EXPECT_FALSE(noSlots.adopt());
}

void ClaimAllSlots(TaskScheduler& scheduler, unsigned remaining) {
  auto slot = scheduler.adopt();
  if (remaining == 0) {
    EXPECT_FALSE(slot);
  } else {
    ASSERT_TRUE(slot);
    ClaimAllSlots(scheduler, remaining - 1);
  }
}

TEST(AdoptedThreadTest, ClaimsAndReleasesAcrossMaskWords) {
  constexpr unsigned kSlots = 66;
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = 1, .dequeCapacity = 2, .adoptableThreads = kSlots});
  ClaimAllSlots(scheduler, kSlots);
  ClaimAllSlots(scheduler, kSlots);
}

TEST(AdoptedThreadTest, ExhaustedCallerRemainsAnOrdinaryNonWorker) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  auto owner = scheduler.adopt();
  ASSERT_TRUE(owner);
  std::thread visitor([&] {
    auto unavailable = scheduler.adopt();
    EXPECT_FALSE(unavailable);
    TaskScope scope(scheduler);
    int ranOn = -1;
    scope.spawn([&] { ranOn = Peer::WorkerIndex(scheduler); });
    scope.wait();
    EXPECT_THAT(ranOn, Eq(0));
  });
  visitor.join();
}

TEST(AdoptedThreadTest, OwnScopeRunsOnTheCallerAndItsSlotRetainsTheRecordBound) {
  Gate worker;
  const auto owner = std::this_thread::get_id();
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .maxTasksPerCreator = 1});
  std::thread holder([&] {
    // These scopes elsewhere must not prevent the caller from adopting.
    TaskScope external(scheduler);
    external.spawn([&] {
      TaskScope unrelated(scheduler);
      worker.arriveAndWait();
    });
  });
  const bool held = worker.waitForArrival();
  {
    auto guard = scheduler.adopt();
    EXPECT_TRUE(guard);
    if (held && guard) {
      const StackBounds platform = CurrentThreadStackBounds();
      const StackBounds captured = Peer::CurrentAdoptedStack(scheduler);
      EXPECT_EQ(captured.floor, platform.floor);
      EXPECT_EQ(captured.size, platform.size);
      EXPECT_GE(StackRemaining(CurrentStackPosition(), captured), kStackReserveBytes);
      TaskScope scope(scheduler);
      std::atomic<int> calls{0};
      EXPECT_TRUE(scope.trySpawn([&] {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        ++calls;
      }));
      EXPECT_THAT(Peer::AdoptedLiveRecords(scheduler), Eq(1u));
      auto extra = [&] { ++calls; };
      EXPECT_FALSE(scope.trySpawn(extra));
      scope.spawn(extra);
      EXPECT_THAT(calls.load(), Eq(1));
      scope.wait();
      EXPECT_THAT(calls.load(), Eq(2));
      EXPECT_THAT(Peer::AdoptedLiveRecords(scheduler), Eq(0u));
      EXPECT_TRUE(scope.trySpawn(extra));
      scope.wait();
      EXPECT_THAT(calls.load(), Eq(3));
    }
  }
  worker.release();
  holder.join();
  EXPECT_TRUE(held);
  EXPECT_FALSE(worker.heldPastHangGuard());
}

TEST(AdoptedThreadTest, GuardDoesNotAdoptTheCallerIntoAnotherScheduler) {
  TaskScheduler adopted(TaskSchedulerOptions{.workerCount = 1});
  TaskScheduler other(TaskSchedulerOptions{.workerCount = 1});
  auto guard = adopted.adopt();
  EXPECT_TRUE(guard);
  TaskScope elsewhere(other);
  int worker = -1;
  elsewhere.spawn([&] { worker = Peer::WorkerIndex(other); });
  elsewhere.wait();
  EXPECT_THAT(worker, Eq(0));
}

void SubmitToOuterAtBound(bool tryOnly, std::size_t dequeCapacity, std::size_t recordBound) {
  Gate busy;
  TaskScheduler scheduler(TaskSchedulerOptions{
      .workerCount = 1, .dequeCapacity = dequeCapacity, .maxTasksPerCreator = recordBound});
  Peer::Spawn(scheduler, [&] { busy.arriveAndWait(); });
  const bool held = busy.waitForArrival();
  {
    auto slot = scheduler.adopt();
    TaskScope outer(scheduler);
    TaskScope inner(scheduler);
    if (held && slot) {
      EXPECT_TRUE(inner.trySpawn([] {}));
      if (tryOnly) {
        (void)outer.trySpawn([] {});
      } else {
        outer.spawn([] {});
      }
    }
  }
  busy.release();
}

TEST(AdoptedThreadDeathTest, OuterSubmissionIsRejectedBeforePoolFailureOrInlineFallback) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(SubmitToOuterAtBound(true, 4, 1), "innermost synchronous scope");
  EXPECT_DEATH(SubmitToOuterAtBound(false, 4, 1), "innermost synchronous scope");
  EXPECT_DEATH(SubmitToOuterAtBound(true, 1, 4), "innermost synchronous scope");
  EXPECT_DEATH(SubmitToOuterAtBound(false, 1, 4), "innermost synchronous scope");
}

TEST(AdoptedThreadDeathTest, JoiningAnOuterScopeWhileAnInnerScopeIsLiveFails) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        auto guard = scheduler.adopt();
        TaskScope outer(scheduler);
        TaskScope inner(scheduler);
        outer.wait();
      },
      "join its innermost synchronous scope first");
}

void AccessOuterAcrossSchedulers(bool adoptSecond, bool wait) {
  TaskScheduler first(TaskSchedulerOptions{.workerCount = 1});
  TaskScheduler second(TaskSchedulerOptions{.workerCount = 1});
  auto firstGuard = first.adopt();
  auto secondGuard = adoptSecond ? second.adopt() : AdoptedThread{};
  TaskScope outer(first);
  TaskScope inner(second);
  if (wait) {
    outer.wait();
  } else {
    (void)outer.trySpawn([] {});
  }
}

TEST(AdoptedThreadDeathTest, StrictNestingIncludesOtherAdoptedAndUnadoptedSchedulers) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  for (const bool adoptSecond : {true, false}) {
    EXPECT_DEATH(AccessOuterAcrossSchedulers(adoptSecond, false), "innermost synchronous scope");
    EXPECT_DEATH(AccessOuterAcrossSchedulers(adoptSecond, true),
                 "join its innermost synchronous scope");
  }
}

TEST(AdoptedThreadTest, OrdinaryCallersCanSubmitOuterWorkAcrossSchedulers) {
  TaskScheduler first(TaskSchedulerOptions{.workerCount = 1});
  TaskScheduler second(TaskSchedulerOptions{.workerCount = 1});
  int result = 0;
  TaskScope outer(first);
  {
    TaskScope inner(second);
    EXPECT_TRUE(outer.trySpawn([&] { result = 17; }));
  }
  outer.wait();
  EXPECT_THAT(result, Eq(17));
}

TEST(AdoptedThreadDeathTest, AdoptedDispatchChecksTheStackReserveBeforeCallingTheTask) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        Gate busy;
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        Peer::Spawn(scheduler, [&] { busy.arriveAndWait(); });
        const bool held = busy.waitForArrival();
        {
          auto guard = scheduler.adopt();
          TaskScope scope(scheduler);
          if (held && guard) {
            scope.spawn([] {});
            Peer::SetAdoptedStackFloor(scheduler, CurrentStackPosition());
            scope.wait();
          }
        }
        busy.release();
      },
      "adopted scheduler thread is out of stack");
}

TEST(AdoptedThreadDeathTest, CannotAdoptWhileAnySynchronousScopeIsLiveOnTheCaller) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler first(TaskSchedulerOptions{.workerCount = 0});
        TaskScheduler second(TaskSchedulerOptions{.workerCount = 1});
        TaskScope scope(first);
        (void)second.adopt();
      },
      "adopt with a live TaskScope");
}

TEST(AdoptedThreadDeathTest, AdoptedRecordsCountTowardTheTotalLiveRecordBound) {
  if constexpr (std::numeric_limits<std::size_t>::digits >= 63) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
          TaskScheduler scheduler(TaskSchedulerOptions{
              .workerCount = 1,
              .maxTasksPerCreator = static_cast<std::size_t>(std::uint64_t{1} << 61),
              .adoptableThreads = 1});
        },
        "live-record bound");
  }
}

TEST(AdoptedThreadDeathTest, AWorkerCannotAdoptEvenOnAnotherScheduler) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler first(TaskSchedulerOptions{.workerCount = 1});
        TaskScheduler second(TaskSchedulerOptions{.workerCount = 1});
        TaskScope scope(first);
        scope.spawn([&] { (void)second.adopt(); });
      },
      "worker cannot adopt");
}

TEST(AdoptedThreadDeathTest, CannotReleaseWithALiveScope) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        std::unique_ptr<AdoptedThread> guard(new AdoptedThread(scheduler.adopt()));
        TaskScope scope(scheduler);
        guard.reset();
      },
      "released with a live TaskScope");
}

TEST(AdoptedThreadDeathTest, ReleaseBelongsToTheThreadThatClaimedTheSlot) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
        std::unique_ptr<AdoptedThread> guard(new AdoptedThread(scheduler.adopt()));
        std::thread other([&] { guard.reset(); });
        other.join();
      },
      "released on a different thread");
}

TEST(AdoptedThreadDeathTest, SchedulerCannotBeDestroyedWhileASlotIsClaimed) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        auto scheduler = std::make_unique<TaskScheduler>(TaskSchedulerOptions{.workerCount = 1});
        auto guard = scheduler->adopt();
        scheduler.reset();
      },
      "destroyed with a live AdoptedThread");
}

}  // namespace
}  // namespace donner::tasks
