/// @file
/// Tests for \ref donner::geode::BoundedGpuWait.
///
/// All timing is driven through the injectable clock/sleep hooks: no test
/// here performs a real sleep, and a stalled wait is simulated by a poll that
/// never observes completion. A true red-run against the previous unbounded
/// behavior is impossible by construction (an unbounded wait on a stalled
/// poll never returns, so the test process would hang forever instead of
/// failing); the bounded contract is pinned here instead.

#include "donner/svg/renderer/geode/GeodeGpuWait.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace donner::geode {
namespace {

using namespace std::chrono_literals;

/// Deterministic fake time source: `now` returns the accumulated fake time,
/// and `sleep` advances it. A wait loop under these hooks makes progress only
/// through its own sleep calls, so poll counts and elapsed fake time are
/// exact.
struct FakeClock {
  std::chrono::steady_clock::time_point current{};
  std::vector<std::chrono::microseconds> sleeps;

  GpuWaitTestHooks hooks() {
    GpuWaitTestHooks result;
    result.now = [this] { return current; };
    result.sleep = [this](std::chrono::microseconds duration) {
      sleeps.push_back(duration);
      current += duration;
    };
    return result;
  }
};

TEST(BoundedGpuWait, CompletesImmediatelyWithoutSleeping) {
  FakeClock clock;
  int pollCount = 0;
  const GpuWaitResult result = BoundedGpuWait(
      [&pollCount] {
        ++pollCount;
        return true;
      },
      5000ms, 100us, clock.hooks());

  EXPECT_EQ(result, GpuWaitResult::Complete);
  EXPECT_EQ(pollCount, 1);
  EXPECT_TRUE(clock.sleeps.empty());
}

TEST(BoundedGpuWait, CompletesAfterSeveralPolls) {
  FakeClock clock;
  int pollCount = 0;
  const GpuWaitResult result = BoundedGpuWait(
      [&pollCount] {
        ++pollCount;
        return pollCount >= 5;
      },
      5000ms, 100us, clock.hooks());

  EXPECT_EQ(result, GpuWaitResult::Complete);
  EXPECT_EQ(pollCount, 5);
  // One sleep between each pending poll and the next.
  EXPECT_EQ(clock.sleeps.size(), 4u);
  for (const auto& duration : clock.sleeps) {
    EXPECT_EQ(duration, 100us);
  }
}

TEST(BoundedGpuWait, StalledPollTimesOutAtTheDeadline) {
  FakeClock clock;
  int pollCount = 0;
  const GpuWaitResult result = BoundedGpuWait(
      [&pollCount] {
        ++pollCount;
        return false;  // Simulated hung device: completion is never observed.
      },
      10ms, 100us, clock.hooks());

  EXPECT_EQ(result, GpuWaitResult::TimedOut);
  // 10 ms budget at a 100 us cadence: exactly 100 sleeps, and the poll after
  // the deadline-crossing sleep is the last one. Exact counts pin that the
  // loop neither spins without sleeping nor overshoots the deadline by more
  // than one interval.
  EXPECT_EQ(clock.sleeps.size(), 100u);
  EXPECT_EQ(pollCount, 101);
  EXPECT_EQ(clock.current.time_since_epoch(), std::chrono::microseconds(10'000));
}

TEST(BoundedGpuWait, ZeroTimeoutPollsOnceThenTimesOut) {
  FakeClock clock;
  int pollCount = 0;
  const GpuWaitResult result = BoundedGpuWait(
      [&pollCount] {
        ++pollCount;
        return false;
      },
      0ms, 100us, clock.hooks());

  EXPECT_EQ(result, GpuWaitResult::TimedOut);
  // Even a zero budget gives the condition one poll (a completed wait must
  // never be reported as a timeout), but sleeps nothing.
  EXPECT_EQ(pollCount, 1);
  EXPECT_TRUE(clock.sleeps.empty());
}

TEST(BoundedGpuWait, ASubMillisecondBudgetIsNotRoundedUp) {
  // The snapshot readback waits one poll interval at a time, so its budget is smaller than a
  // millisecond. A budget expressed in whole milliseconds would round a 100 us slice up to
  // 1 ms and coarsen the poll cadence tenfold, which is a cadence the readback path is tuned to
  // and the perf ceilings assume.
  FakeClock clock;
  int pollCount = 0;
  const GpuWaitResult result = BoundedGpuWait(
      [&pollCount] {
        ++pollCount;
        return false;
      },
      100us, 100us, clock.hooks());

  EXPECT_EQ(result, GpuWaitResult::TimedOut);
  // One sleep of the whole budget, then the poll that observes the deadline. A budget rounded up
  // to a millisecond would sleep ten times instead.
  EXPECT_EQ(clock.sleeps.size(), 1u);
  EXPECT_EQ(pollCount, 2);
  EXPECT_EQ(clock.current.time_since_epoch(), std::chrono::microseconds(100));
}

/// A device-lost record starts with no timeout attribution, so "lost with no
/// site" is a meaningful state: it says the driver reported the loss rather
/// than a deadline discovering it.
TEST(GeodeDeviceLostState, StartsWithNoTimeoutAttribution) {
  const GeodeDeviceLostState state;

  EXPECT_FALSE(state.lost.load());
  EXPECT_EQ(state.timedOutSite.load(), GpuWaitSite::None);
  EXPECT_EQ(state.timedOutElapsedMs.load(), 0);
}

/// Only the declaring call records an attribution, so a wait that expires
/// against an already-hung device reports nothing new: it inherited the hang
/// rather than finding it.
TEST(GeodeDeviceLostState, OnlyTheDeclaringWaitRecordsAnAttribution) {
  GeodeDeviceLostState state;

  EXPECT_TRUE(DeclareDeviceLostAfterWaitTimeout(state, GpuWaitSite::ReadbackMap, 10000ms));
  EXPECT_FALSE(DeclareDeviceLostAfterWaitTimeout(state, GpuWaitSite::QueueIdle, 5000ms));

  EXPECT_TRUE(state.lost.load());
  EXPECT_EQ(state.timedOutSite.load(), GpuWaitSite::ReadbackMap);
  EXPECT_EQ(state.timedOutElapsedMs.load(), 10000);
}

/// A driver-reported loss leaves the attribution empty, and a wait that
/// expires afterwards must not fill it in: an empty site is the only way a
/// report can say the driver declared the loss.
TEST(GeodeDeviceLostState, ADriverLossKeepsTheAttributionEmpty) {
  GeodeDeviceLostState state;

  EXPECT_TRUE(DeclareDeviceLost(state));
  EXPECT_FALSE(DeclareDeviceLostAfterWaitTimeout(state, GpuWaitSite::ReadbackMap, 10000ms));

  EXPECT_EQ(state.timedOutSite.load(), GpuWaitSite::None);
  EXPECT_EQ(state.timedOutElapsedMs.load(), 0);
}

/// The two declaring paths run on different threads (the driver's device-lost
/// callback is spontaneous; bounded waits expire on whichever thread is
/// waiting), so they race for real. Whoever wins the flag owns the
/// attribution, and the loser must leave it alone - a check-then-set that
/// reads the flag and then stores the site lets a driver loss landing in the
/// gap be relabelled as a wait timeout, which this pins against by asserting
/// the attribution always agrees with the reported winner.
TEST(GeodeDeviceLostState, ConcurrentDriverLossAndWaitTimeoutAgreeOnOneAttribution) {
  constexpr int kTrials = 1000;
  for (int trial = 0; trial < kTrials; ++trial) {
    GeodeDeviceLostState state;
    std::atomic<bool> go{false};
    bool driverDeclared = false;
    bool waitDeclared = false;

    std::thread driver([&] {
      while (!go.load(std::memory_order_acquire)) {}
      driverDeclared = DeclareDeviceLost(state);
    });
    std::thread waiter([&] {
      while (!go.load(std::memory_order_acquire)) {}
      waitDeclared = DeclareDeviceLostAfterWaitTimeout(state, GpuWaitSite::ReadbackMap, 10000ms);
    });
    go.store(true, std::memory_order_release);
    driver.join();
    waiter.join();

    ASSERT_TRUE(state.lost.load());
    ASSERT_NE(driverDeclared, waitDeclared) << "exactly one caller may declare the loss";
    if (driverDeclared) {
      ASSERT_EQ(state.timedOutSite.load(), GpuWaitSite::None)
          << "a driver-reported loss was relabelled as a wait timeout (trial " << trial << ")";
      ASSERT_EQ(state.timedOutElapsedMs.load(), 0);
    } else {
      ASSERT_EQ(state.timedOutSite.load(), GpuWaitSite::ReadbackMap)
          << "the declaring wait lost its attribution (trial " << trial << ")";
      ASSERT_EQ(state.timedOutElapsedMs.load(), 10000)
          << "the site was published without its elapsed time (trial " << trial << ")";
    }
  }
}

TEST(BoundedGpuWait, ZeroPollIntervalDoesNotSleep) {
  FakeClock clock;
  int pollCount = 0;
  // A zero interval must not call the sleep hook at all (busy-poll mode used
  // by tests); the deadline still bounds the loop because the fake clock is
  // advanced manually here through `now`.
  GpuWaitTestHooks hooks = clock.hooks();
  hooks.now = [&clock, &pollCount] {
    // Advance fake time 1 ms per deadline check so the loop terminates.
    clock.current += 1ms;
    (void)pollCount;
    return clock.current;
  };
  const GpuWaitResult result = BoundedGpuWait(
      [&pollCount] {
        ++pollCount;
        return false;
      },
      5ms, 0us, hooks);

  EXPECT_EQ(result, GpuWaitResult::TimedOut);
  EXPECT_TRUE(clock.sleeps.empty());
  EXPECT_GE(pollCount, 1);
}

}  // namespace
}  // namespace donner::geode
