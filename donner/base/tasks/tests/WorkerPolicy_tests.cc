/// @file
/// Worker sizing, partial startup and per-thread scheduling policy.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <limits>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::ElementsAre;
using testing::Eq;

class RefuseStarts : public SchedulerTestHooks {
public:
  explicit RefuseStarts(unsigned allowed) : allowed_(allowed) {}

  void at(SchedulerStep, std::size_t) override {}

  WorkerThreadCreate workerThreadCreate(std::size_t attempt) override {
    ++attempts;
    return (allowed_ & (1u << attempt)) != 0 ? &pthread_create : &Refuse;
  }

  std::size_t attempts = 0;

private:
  static int Refuse(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) { return EAGAIN; }
  const unsigned allowed_;
};

TEST(WorkerPolicyTest, WorkerCountReservesOneProcessorAndClampsAtBothEnds) {
  EXPECT_THAT(detail::WorkerCountForProcessors(0), Eq(1u));
  EXPECT_THAT(detail::WorkerCountForProcessors(1), Eq(1u));
  EXPECT_THAT(detail::WorkerCountForProcessors(2), Eq(1u));
  EXPECT_THAT(detail::WorkerCountForProcessors(3), Eq(2u));
  EXPECT_THAT(detail::WorkerCountForProcessors(32), Eq(31u));
  EXPECT_THAT(detail::WorkerCountForProcessors(33), Eq(32u));
  EXPECT_THAT(detail::WorkerCountForProcessors(std::numeric_limits<std::size_t>::max()), Eq(32u));
  EXPECT_THAT(TaskSchedulerOptions{}.workerCount, Eq(DefaultWorkerCount()));
}

TEST(WorkerPolicyTest, FailureOfEveryStartRunsInlineWithoutChangingTheConfiguration) {
  RefuseStarts hooks(0);
  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 3}, &hooks);
  EXPECT_THAT(hooks.attempts, Eq(3u));
  EXPECT_THAT(scheduler->workerCount(), Eq(0u));
  EXPECT_THAT(scheduler->schedulingWidth(), Eq(0u));
  EXPECT_FALSE(scheduler->threadingDisabledByConfiguration());
  EXPECT_TRUE(scheduler->schedule().await_ready());
  const auto owner = std::this_thread::get_id();
  int calls = 0;
  TaskScope scope(*scheduler);
  EXPECT_FALSE(scope.trySpawn([&] { ++calls; }));
  scope.spawn([&] {
    EXPECT_THAT(std::this_thread::get_id(), Eq(owner));
    ++calls;
  });
  EXPECT_THAT(calls, Eq(1));
}

TEST(WorkerPolicyTest, FailuresBetweenStartsLeaveOnlyDenseLiveWorkerIndices) {
  RefuseStarts hooks(0b01010);
  auto scheduler =
      Peer::Make(TaskSchedulerOptions{.workerCount = 5,
                                      .idleSpinRounds = std::numeric_limits<std::size_t>::max()},
                 &hooks);
  EXPECT_THAT(hooks.attempts, Eq(5u));
  EXPECT_THAT(scheduler->workerCount(), Eq(2u));
  EXPECT_THAT(scheduler->schedulingWidth(), Eq(2u));
  EXPECT_FALSE(scheduler->threadingDisabledByConfiguration());
  std::array<int, 2> seen{-1, -1};
  Gate first;
  TaskScope scope(*scheduler);
  scope.spawn([&] {
    seen[0] = Peer::WorkerIndex(*scheduler);
    first.arriveAndWait();
  });
  const bool held = first.waitForArrival();
  Event second;
  scope.spawn([&] {
    seen[1] = Peer::WorkerIndex(*scheduler);
    second.set();
  });
  const bool bothRunning = second.wait();
  first.release();
  scope.wait();
  EXPECT_TRUE(held);
  EXPECT_TRUE(bothRunning);
  EXPECT_FALSE(first.heldPastHangGuard());
  std::sort(seen.begin(), seen.end());
  EXPECT_THAT(seen, ElementsAre(0, 1));
}

TEST(WorkerPolicyTest, FirstWorkerAcquiresTheCompletedStartupRoster) {
  struct StartupHooks : SchedulerTestHooks {
    WorkerThreadCreate workerThreadCreate(std::size_t attempt) override {
      if (attempt == 1) {
        sawFirstAtBarrier =
            PollUntil([&] { return firstAtBarrier.load(std::memory_order_relaxed); });
        // Only the startup mutex publishes this ordinary constructor write to worker zero.
        lastAttempt = attempt;
      }
      return &pthread_create;
    }

    void at(SchedulerStep step, std::size_t worker) override {
      if (worker == 0 && step == SchedulerStep::WorkerBeforeStartupBarrier) {
        firstAtBarrier.store(true, std::memory_order_relaxed);
      } else if (worker == 0 && step == SchedulerStep::BeforeSleeperWaits) {
        observedAttempt = lastAttempt;
        firstParked.store(true, std::memory_order_relaxed);
      }
    }

    std::atomic<bool> firstAtBarrier{false};
    std::atomic<bool> firstParked{false};
    bool sawFirstAtBarrier = false;
    std::size_t lastAttempt = 0;
    std::size_t observedAttempt = 0;
  } hooks;

  auto scheduler = Peer::Make(TaskSchedulerOptions{.workerCount = 2}, &hooks);
  const bool parked = PollUntil([&] { return hooks.firstParked.load(std::memory_order_relaxed); });
  EXPECT_THAT(scheduler->workerCount(), Eq(2u));
  scheduler.reset();
  EXPECT_TRUE(hooks.sawFirstAtBarrier);
  EXPECT_TRUE(parked);
  EXPECT_THAT(hooks.observedAttempt, Eq(1u));
}

#if defined(__linux__)
TEST(WorkerPolicyTest, DefaultUsesTheCallingThreadsActualLinuxAffinity) {
  // A separate thread owns the narrowed affinity for its entire remaining lifetime.
  std::thread probe([] {
    std::vector<cpu_set_t> original(1);
    while (sched_getaffinity(0, original.size() * sizeof(cpu_set_t), original.data()) != 0) {
      ASSERT_THAT(errno, Eq(EINVAL));
      original.resize(original.size() * 2);
    }
    const std::size_t bytes = original.size() * sizeof(cpu_set_t);
    std::vector<cpu_set_t> subset(original.size());
    CPU_ZERO_S(bytes, subset.data());
    std::size_t selected = 0;
    for (std::size_t i = 0; i < bytes * CHAR_BIT && selected < 3; ++i) {
      if (CPU_ISSET_S(i, bytes, original.data())) {
        CPU_SET_S(i, bytes, subset.data());
        ++selected;
        ASSERT_THAT(sched_setaffinity(0, bytes, subset.data()), Eq(0));
        EXPECT_THAT(DefaultWorkerCount(), Eq(selected == 3 ? 2u : 1u));
      }
    }
    EXPECT_GT(selected, 0u);
  });
  probe.join();
}
#elif defined(__APPLE__)
TEST(WorkerPolicyTest, AppleWorkersApplyTheRequestedClassAndCallerKeepsItsClass) {
  const auto caller = qos_class_self();
  EXPECT_THAT(TaskSchedulerOptions{}.workerQos, Eq(WorkerQos::kInteractive));
  for (auto [requested, expected] : {std::pair{WorkerQos::kInteractive, QOS_CLASS_USER_INTERACTIVE},
                                     std::pair{WorkerQos::kUserInitiated, QOS_CLASS_USER_INITIATED},
                                     std::pair{WorkerQos::kDefault, QOS_CLASS_DEFAULT}}) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .workerQos = requested});
    qos_class_t observed = QOS_CLASS_UNSPECIFIED;
    TaskScope scope(scheduler);
    scope.spawn([&] { observed = qos_class_self(); });
    scope.wait();
    EXPECT_THAT(observed, Eq(expected));
    EXPECT_THAT(qos_class_self(), Eq(caller));
  }
}
#endif

}  // namespace
}  // namespace donner::tasks
