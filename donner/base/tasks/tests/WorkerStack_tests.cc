#include "donner/base/tasks/WorkerStack.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "donner/base/tasks/TaskScheduler.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;
using testing::AllOf;
using testing::Each;
using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::Le;
using testing::Lt;
using testing::Optional;

/// One worker's view of its stack, taken inside a task.
struct StackProbe {
  std::uintptr_t position = 0;
  StackBounds platform;  //!< As the platform reports the thread's stack.
  StackBounds bounds;    //!< As the worker recorded it.
  std::size_t remaining = 0;
};

std::ostream& operator<<(std::ostream& os, const StackProbe& probe) {
  return os << "{position " << probe.position << ", platform floor " << probe.platform.floor
            << " size " << probe.platform.size << ", recorded floor " << probe.bounds.floor
            << " size " << probe.bounds.size << ", remaining " << probe.remaining << "}";
}

/// Runs \p body as one task on a worker of a new scheduler and waits for it under the hang guard.
/// Everything the task touches is owned by the shared state, so a task still running after a
/// failed wait touches nothing out of scope.
template <typename State, typename Body>
bool RunOnAWorker(const std::shared_ptr<State>& state, TaskScheduler& scheduler, Body body) {
  auto done = std::make_shared<Event>();
  Peer::Spawn(scheduler, [state, done, body] {
    body(*state);
    done->set();
  });
  return done->wait();
}

/**
 * Recurses while \p keepGoing(depth, remaining) holds, each level taking \p step bytes of real
 * stack, records the stack left at each level, and calls \p atBottom at the deepest level.
 */
[[gnu::noinline]] void Descend(std::size_t step,
                               const std::function<bool(int, std::size_t)>& keepGoing,
                               std::vector<std::size_t>& remaining, int depth,
                               const std::function<void()>& atBottom = {}) {
  // An alloca lives on the real stack under every sanitizer, unlike a local array, which
  // AddressSanitizer's fake stack can move to the heap.
  auto* block = static_cast<volatile char*>(__builtin_alloca(step));
  block[0] = 1;
  block[step - 1] = 1;
  const std::size_t left = Peer::StackRemaining().value_or(0);
  remaining.push_back(left);
  if (keepGoing(depth, left)) {
    Descend(step, keepGoing, remaining, depth + 1, atBottom);
  } else if (atBottom) {
    atBottom();
  }
  // Keeps the block live across the call, so the recursion is not a tail call.
  block[0] = 0;
}

TEST(WorkerStackTest, RemainingIsTheDistanceAboveTheFloorAndNeverNegative) {
  const StackBounds bounds{.floor = 0x10000, .size = 0x8000};
  EXPECT_THAT(StackRemaining(0x14000, bounds), Eq(0x4000u));
  EXPECT_THAT(StackRemaining(0x10000, bounds), Eq(0u)) << "at the floor";
  EXPECT_THAT(StackRemaining(0x0f000, bounds), Eq(0u)) << "below the floor clamps to zero";
}

TEST(WorkerStackTest, AWorkerUsesTheTopOfItsStackUpToTheConfiguredSize) {
  const StackBounds platform{.floor = 0x100000, .size = 0x80000};
  EXPECT_THAT(UsableStack(platform, 0x20000).floor, Eq(0x160000u)) << "a larger platform stack";
  EXPECT_THAT(UsableStack(platform, 0x20000).size, Eq(0x20000u));
  EXPECT_THAT(UsableStack(platform, 0x100000).floor, Eq(0x100000u)) << "a smaller platform stack";
  EXPECT_THAT(UsableStack(platform, 0x100000).size, Eq(0x80000u));
}

TEST(WorkerStackTest, AWorkerStackIsWholePagesAndAtLeastTheMinimum) {
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  EXPECT_THAT(WorkerStackSize(512 * 1024) % page, Eq(0u));
  EXPECT_THAT(WorkerStackSize(512 * 1024 + 1), Eq((512 * 1024 + 1 + page - 1) / page * page));
  EXPECT_THAT(WorkerStackSize(512 * 1024), Ge(512u * 1024));
}

TEST(WorkerStackDeathTest, AStackBelowTheMinimumFails) {
  EXPECT_DEATH((void)WorkerStackSize(kStackReserveBytes), "at least the platform minimum");
  EXPECT_DEATH(TaskScheduler(TaskSchedulerOptions{.workerCount = 1, .workerStackBytes = 4096}),
               "at least the platform minimum");
}

class StackWorkerCounts : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(Workers, StackWorkerCounts, testing::Values(1, 4),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                           return "Workers" + std::to_string(info.param);
                         });

// Every worker records its stack when it starts: a task's frame lies inside the recorded bounds,
// and the bounds have the configured size.
TEST_P(StackWorkerCounts, ATasksFrameLiesWithinItsWorkersRecordedBounds) {
  constexpr std::size_t kStackBytes = 384 * 1024;
  constexpr int kTasks = 64;
  struct State {
    std::vector<StackProbe> probes = std::vector<StackProbe>(kTasks);
  };
  auto state = std::make_shared<State>();
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = GetParam(), .workerStackBytes = kStackBytes});
  for (int i = 0; i < kTasks; ++i) {
    EXPECT_TRUE(RunOnAWorker(state, scheduler,
                             [&scheduler, i](State& s) {
                               StackProbe& probe = s.probes[static_cast<std::size_t>(i)];
                               probe.position = CurrentStackPosition();
                               probe.platform = CurrentThreadStackBounds();
                               probe.bounds = Peer::CurrentWorkerStack(scheduler);
                               probe.remaining = Peer::StackRemaining().value_or(0);
                             }))
        << "task " << i;
  }
  for (const StackProbe& probe : state->probes) {
    EXPECT_THAT(probe.position,
                AllOf(Gt(probe.bounds.floor), Lt(probe.bounds.floor + probe.bounds.size)))
        << probe;
    EXPECT_THAT(probe.platform.size, Ge(WorkerStackSize(kStackBytes)))
        << "the platform gives at least the configured size; " << probe;
    EXPECT_THAT(probe.bounds.size, Eq(WorkerStackSize(kStackBytes)))
        << "the worker counts only the configured size; " << probe;
    EXPECT_THAT(probe.bounds.floor + probe.bounds.size,
                Eq(probe.platform.floor + probe.platform.size))
        << "from the top of the platform's stack; " << probe;
    EXPECT_THAT(probe.remaining, AllOf(Gt(probe.bounds.size / 2), Lt(probe.bounds.size)))
        << "a task starts near the top of its worker's stack; " << probe;
  }
}

// A stack larger than any platform's default for a new thread is granted, so the size reaches the
// platform rather than the default being used.
TEST(WorkerStackTest, AStackLargerThanThePlatformDefaultIsGranted) {
  constexpr std::size_t kStackBytes = 16 * 1024 * 1024;
  struct State {
    StackProbe probe;
  };
  auto state = std::make_shared<State>();
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .workerStackBytes = kStackBytes});
  ASSERT_TRUE(RunOnAWorker(state, scheduler, [&scheduler](State& s) {
    s.probe.platform = CurrentThreadStackBounds();
    s.probe.bounds = Peer::CurrentWorkerStack(scheduler);
  }));
  EXPECT_THAT(state->probe.platform.size, Ge(kStackBytes)) << state->probe;
  EXPECT_THAT(state->probe.bounds.size, Eq(WorkerStackSize(kStackBytes))) << state->probe;
}

// The stack left falls by about the frame size at each level of a recursion.
TEST(WorkerStackTest, RemainingFallsByAboutTheFrameSizeWithEachLevel) {
  constexpr std::size_t kStep = 4096;
  constexpr int kLevels = 16;
  struct State {
    std::vector<std::size_t> remaining;
  };
  auto state = std::make_shared<State>();
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  ASSERT_TRUE(RunOnAWorker(state, scheduler, [](State& s) {
    Descend(kStep, [](int depth, std::size_t) { return depth + 1 < kLevels; }, s.remaining, 0);
  }));
  ASSERT_THAT(state->remaining.size(), Eq(static_cast<std::size_t>(kLevels)));
  std::vector<std::size_t> steps;
  for (std::size_t i = 1; i < state->remaining.size(); ++i) {
    steps.push_back(state->remaining[i - 1] - state->remaining[i]);
  }
  EXPECT_THAT(steps, Each(AllOf(Ge(kStep), Le(kStep + 1024))))
      << "each level takes its block plus a frame of bounded size";
  RecordProperty("frame_bytes_per_level", std::to_string(steps.front() - kStep));
}

// The accessor reads the frame address, which is on the real stack in every build. The address of
// a local is not: AddressSanitizer's fake stack, when enabled, puts locals on the heap. The case
// records where a local was, for the build it ran in.
TEST(WorkerStackTest, TheAccessorReadsTheRealStackWhereverLocalsLive) {
  struct State {
    StackProbe probe;
    bool localOnTheRealStack = false;
  };
  auto state = std::make_shared<State>();
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  ASSERT_TRUE(RunOnAWorker(state, scheduler, [&scheduler](State& s) {
    volatile char local[64] = {};
    local[0] = 1;
    s.probe.bounds = Peer::CurrentWorkerStack(scheduler);
    s.probe.position = CurrentStackPosition();
    s.probe.remaining = Peer::StackRemaining().value_or(0);
    const auto address = reinterpret_cast<std::uintptr_t>(&local[0]);
    s.localOnTheRealStack =
        address > s.probe.bounds.floor && address < s.probe.bounds.floor + s.probe.bounds.size;
  }));
  const StackProbe& probe = state->probe;
  EXPECT_THAT(probe.position,
              AllOf(Gt(probe.bounds.floor), Lt(probe.bounds.floor + probe.bounds.size)))
      << "the frame address is on the real stack; " << probe;
  EXPECT_THAT(probe.remaining, AllOf(Gt(0u), Lt(probe.bounds.size))) << probe;
  RecordProperty("local_on_real_stack", state->localOnTheRealStack ? "yes" : "no");
}

// The main thread and other threads that are not workers report no worker stack.
TEST(WorkerStackTest, ThreadsThatAreNotWorkersReportNoWorkerStack) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  EXPECT_THAT(Peer::StackRemaining(), Eq(std::nullopt)) << "the main thread";
  std::optional<std::size_t> onOtherThread = 0;
  std::thread other([&] { onOtherThread = Peer::StackRemaining(); });
  other.join();
  EXPECT_THAT(onOtherThread, Eq(std::nullopt)) << "a thread that is not a worker";
  struct State {
    std::optional<std::size_t> onWorker;
  };
  auto state = std::make_shared<State>();
  ASSERT_TRUE(
      RunOnAWorker(state, scheduler, [](State& s) { s.onWorker = Peer::StackRemaining(); }));
  EXPECT_THAT(state->onWorker, Optional(Gt(0u))) << "a worker reports its stack";
}

// The configured size takes effect: a recursion that needs 384 KiB fits on a 1 MiB worker stack and
// not on a 256 KiB one. The recursion stops by the accessor, never by overflowing.
TEST(WorkerStackTest, TheConfiguredStackSizeTakesEffect) {
  constexpr std::size_t kStep = 4096;
  constexpr int kNeededLevels = (384 * 1024) / kStep;
  struct State {
    std::vector<std::size_t> remaining;
  };
  auto depthOn = [&](std::size_t stackBytes) {
    auto state = std::make_shared<State>();
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .workerStackBytes = stackBytes});
    const bool finished = RunOnAWorker(state, scheduler, [](State& s) {
      Descend(
          kStep,
          [](int depth, std::size_t left) {
            return depth + 1 < kNeededLevels && left > kStackReserveBytes + 4 * kStep;
          },
          s.remaining, 0);
    });
    EXPECT_TRUE(finished) << stackBytes;
    return static_cast<int>(state->remaining.size());
  };
  EXPECT_THAT(depthOn(1024 * 1024), Eq(kNeededLevels)) << "fits in 1 MiB";
  EXPECT_THAT(depthOn(256 * 1024), Lt(kNeededLevels)) << "does not fit in 256 KiB";
}

// A worker that would start a task with less than the reserve left fails a release assertion
// instead of overflowing; with more left, the task runs.
TEST(WorkerStackTest, ATaskRunsWhenTheReserveIsLeft) {
  struct State {
    bool ran = false;
  };
  auto state = std::make_shared<State>();
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1});
  ASSERT_TRUE(RunOnAWorker(state, scheduler, [&scheduler](State& s) {
    Peer::RunAsTaskHere(scheduler, [&s] { s.ran = true; });
  }));
  EXPECT_TRUE(state->ran);
}

TEST(WorkerStackDeathTest, ATaskStartedBelowTheReserveFails) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        auto scheduler = std::make_shared<TaskScheduler>(TaskSchedulerOptions{.workerCount = 1});
        struct State {
          std::vector<std::size_t> remaining;
        };
        auto state = std::make_shared<State>();
        (void)RunOnAWorker(state, *scheduler, [scheduler](State& s) {
          // Descend into the reserve, then start a task there.
          Descend(
              1024, [](int, std::size_t left) { return left > kStackReserveBytes - 8 * 1024; },
              s.remaining, 0, [scheduler] { Peer::RunAsTaskHere(*scheduler, [] {}); });
        });
      },
      "out of stack");
}

}  // namespace
}  // namespace donner::tasks
