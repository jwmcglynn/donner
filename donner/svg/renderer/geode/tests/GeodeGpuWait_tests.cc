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

#include <chrono>
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

/// A device-lost record starts with no timeout attribution, so "lost with no
/// site" is a meaningful state: it says the driver reported the loss rather
/// than a deadline discovering it.
TEST(GeodeDeviceLostState, StartsWithNoTimeoutAttribution) {
  const GeodeDeviceLostState state;

  EXPECT_FALSE(state.lost.load());
  EXPECT_EQ(state.timedOutSite.load(), GpuWaitSite::None);
  EXPECT_EQ(state.timedOutElapsedMs.load(), 0);
}

/// A stalled wait must hand its caller both a `TimedOut` result and an elapsed
/// time worth reporting. Without the elapsed measurement a diagnostic consumer
/// could only echo the configured budget back, which would be indistinguishable
/// from a wait that never ran.
TEST(BoundedGpuWait, StalledWaitYieldsAnElapsedTimeWorthRecording) {
  FakeClock clock;
  const auto waitStart = clock.current;
  const GpuWaitResult result = BoundedGpuWait([] { return false; }, 10ms, 100us, clock.hooks());
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(clock.current - waitStart);

  ASSERT_EQ(result, GpuWaitResult::TimedOut);

  // Record it the way a device does when a bounded wait expires.
  GeodeDeviceLostState state;
  state.timedOutSite.store(GpuWaitSite::ReadbackMap);
  state.timedOutElapsedMs.store(static_cast<int>(elapsed.count()));
  state.lost.store(true);

  EXPECT_EQ(state.timedOutSite.load(), GpuWaitSite::ReadbackMap);
  EXPECT_EQ(state.timedOutElapsedMs.load(), 10);
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
