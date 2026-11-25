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

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

TEST(WorkerPolicyTest, RefusedQosIsCountedAndTheWorkerStillCompletesWork) {
  struct RefuseQos : SchedulerTestHooks {
    void at(SchedulerStep, std::size_t) override {}
    int setWorkerQos(WorkerQos qos) override {
      EXPECT_THAT(qos, Eq(WorkerQos::kUserInitiated));
      return EPERM;
    }
  } hooks;
  auto scheduler = Peer::Make(
      TaskSchedulerOptions{.workerCount = 1, .workerQos = WorkerQos::kUserInitiated}, &hooks);
  const auto caller = std::this_thread::get_id();
  std::thread::id runner;
  TaskScope scope(*scheduler);
  scope.spawn([&] { runner = std::this_thread::get_id(); });
  scope.wait();
  EXPECT_NE(runner, caller);
  EXPECT_THAT(scheduler->workerCount(), Eq(1u));
  EXPECT_THAT(scheduler->workerQosRefusals(), Eq(1u));
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
class CgroupQuotaTest : public testing::Test {
protected:
  void SetUp() override {
    std::string pattern = testing::TempDir() + "/worker-quota-XXXXXX";
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    directory = pattern;
  }

  void TearDown() override {
    std::error_code error;
    if (!directory.empty()) {
      std::filesystem::remove_all(directory, error);
      EXPECT_FALSE(error);
    }
  }

  void file(const std::filesystem::path& relative, const std::string& value) {
    const auto path = directory / relative;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    ASSERT_FALSE(error);
    std::ofstream output(path);
    output << value;
    ASSERT_TRUE(output);
  }

  std::size_t available(const std::string& groups, const std::string& mounts,
                        std::size_t affinity = 16) {
    std::istringstream membership(groups);
    std::istringstream mountInfo(mounts);
    return detail::CgroupLimitedProcessors(affinity, membership, mountInfo);
  }

  std::string mount(const std::string& root, const std::string& point, const std::string& type) {
    return "31 25 0:27 " + root + " " + directory.string() + "/" + point +
           " rw,nosuid shared:8 - " + type + "\n";
  }

  std::filesystem::path directory;
};

TEST_F(CgroupQuotaTest, UnifiedHierarchyLimitsAffinityByTheTightestVisibleQuota) {
  const auto mounts = mount("/delegated", "control\\040group", "cgroup2 cgroup rw");
  const std::string groups = "0::/delegated/parent/leaf\n";
  file("control group/cpu.max", "max 100000\n");
  file("control group/parent/cpu.max", "250000 100000\n");
  file("control group/parent/leaf/cpu.max", "900000 100000\n");
  EXPECT_THAT(available(groups, mounts), Eq(3u));
  EXPECT_THAT(available(groups, mounts, 2), Eq(2u));
  file("control group/parent/leaf/cpu.max", "50000 100000\n");
  EXPECT_THAT(available(groups, mounts), Eq(1u));
  EXPECT_THAT(available("0::/delegated-other/parent/leaf\n", mounts), Eq(16u));
}

TEST_F(CgroupQuotaTest, LegacyCpuControllerUsesItsOwnQuotaAndPeriodAtEveryLevel) {
  const auto mounts = mount("/", "sets", "cgroup cgroup rw,cpuset") +
                      mount("/services", "cpu", "cgroup cgroup rw,cpuacct,cpu") +
                      mount("/", "unified", "cgroup2 cgroup rw");
  const std::string groups = "4:cpuset:/unrelated\n2:cpu,cpuacct:/services/work\n0::/v2\n";
  file("sets/unrelated/cpu.cfs_quota_us", "1");
  file("sets/unrelated/cpu.cfs_period_us", "1");
  file("cpu/cpu.cfs_quota_us", "500000");
  file("cpu/cpu.cfs_period_us", "100000");
  file("cpu/work/cpu.cfs_quota_us", "-1");
  file("cpu/work/cpu.cfs_period_us", "100000");
  EXPECT_THAT(available(groups, mounts), Eq(5u));
  file("cpu/work/cpu.cfs_quota_us", "100000");
  file("cpu/work/cpu.cfs_period_us", "50000");
  EXPECT_THAT(available(groups, mounts), Eq(2u));
}

TEST_F(CgroupQuotaTest, UnlimitedMissingAndMalformedQuotasRetainTheAffinityBound) {
  const auto mounts = mount("/", "cg", "cgroup2 cgroup rw");
  const std::string groups = "0::/\n";
  EXPECT_THAT(available(groups, mounts, 8), Eq(8u));
  for (const std::string value : {"max 100000", "abc 100000", "125000 0", "-1 3", "7x 3"}) {
    file("cg/cpu.max", value);
    EXPECT_THAT(available(groups, mounts, 8), Eq(8u)) << value;
  }
  file("cg/cpu.max", "18446744073709551615 18446744073709551614");
  EXPECT_THAT(available(groups, mounts, 8), Eq(2u));
}

TEST(WorkerPolicyTest, DefaultUsesTheCallingThreadsActualLinuxAffinity) {
  // A separate thread owns the narrowed affinity for its entire remaining lifetime.
  std::thread probe([] {
    std::vector<cpu_set_t> original(1);
    while (sched_getaffinity(0, original.size() * sizeof(cpu_set_t), original.data()) != 0) {
      ASSERT_THAT(errno, Eq(EINVAL));
      original.resize(original.size() * 2);
    }
    const std::size_t bytes = original.size() * sizeof(cpu_set_t);
    const auto before = DefaultWorkerCount();
    std::vector<cpu_set_t> subset(original.size());
    CPU_ZERO_S(bytes, subset.data());
    std::size_t selected = 0;
    for (std::size_t i = 0; i < bytes * CHAR_BIT && selected < 3; ++i) {
      if (CPU_ISSET_S(i, bytes, original.data())) {
        CPU_SET_S(i, bytes, subset.data());
        ++selected;
        ASSERT_THAT(sched_setaffinity(0, bytes, subset.data()), Eq(0));
        EXPECT_THAT(DefaultWorkerCount(),
                    Eq(std::min(before, detail::WorkerCountForProcessors(selected))));
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
    if (scheduler.workerQosRefusals() == 0) {
      EXPECT_THAT(observed, Eq(expected));
    } else {
      EXPECT_THAT(scheduler.workerQosRefusals(), Eq(1u));
    }
    EXPECT_THAT(qos_class_self(), Eq(caller));
  }
}
#endif

}  // namespace
}  // namespace donner::tasks
