#include "donner/base/tests/StallGuard.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <optional>
#include <thread>

namespace donner::tests {
namespace {

using std::chrono::steady_clock;
using testing::Eq;
using testing::Optional;

// The guard is a hang bound of tens of seconds, never a latency budget. It must expire strictly
// before the 60 s per-case watchdog in gtest_timeout_main and the 60 s Bazel timeout of a `small`
// test, so a hang fails with the guard's message rather than a bare timeout.
static_assert(kHangGuard >= std::chrono::seconds(10));
static_assert(kHangGuard < std::chrono::seconds(60));

TEST(StallGuardTest, HangGuardWindowIsKHangGuardFromConstruction) {
  const auto before = steady_clock::now();
  const HangGuard guard;
  const auto after = steady_clock::now();

  EXPECT_TRUE(guard.active());
  EXPECT_GE(guard.deadline(), before + kHangGuard);
  EXPECT_LE(guard.deadline(), after + kHangGuard);
}

TEST(StallGuardTest, PollForResultReturnsTheFirstEngagedValue) {
  int polls = 0;
  const auto result = PollForResult([&]() -> std::optional<int> {
    ++polls;
    return polls == 3 ? std::optional<int>(42) : std::nullopt;
  });

  EXPECT_THAT(result, Optional(Eq(42)));
  EXPECT_EQ(polls, 3);
}

TEST(StallGuardTest, PollForResultGivesUpAtAnExpiredDeadline) {
  int polls = 0;
  const auto result = PollForResult(
      [&]() -> std::optional<int> {
        ++polls;
        return 1;
      },
      steady_clock::now() - std::chrono::seconds(1));

  EXPECT_EQ(result, std::nullopt);
  EXPECT_EQ(polls, 0);
}

TEST(StallGuardTest, WaitUntilChecksTheConditionOnceMoreAtTheDeadline) {
  const auto expired = steady_clock::now() - std::chrono::seconds(1);

  EXPECT_TRUE(WaitUntil([] { return true; }, expired));
  EXPECT_FALSE(WaitUntil([] { return false; }, expired));
}

TEST(StallGuardTest, WaitUntilObservesAnotherThreadUnderTheGuard) {
  std::latch started(1);
  std::atomic<bool> done = false;
  std::thread worker([&] {
    started.wait();
    done.store(true, std::memory_order_release);
  });

  started.count_down();
  EXPECT_TRUE(WaitUntil([&] { return done.load(std::memory_order_acquire); }));
  worker.join();
}

TEST(StallGuardTest, PollUntilPumpsUntilTheConditionHolds) {
  int pumps = 0;

  EXPECT_TRUE(PollUntil([&] { ++pumps; }, [&] { return pumps == 5; }));
  EXPECT_EQ(pumps, 5);
}

TEST(StallGuardTest, PollUntilReportsAnExpiredDeadlineWithoutPumping) {
  int pumps = 0;

  EXPECT_FALSE(PollUntil([&] { ++pumps; }, [] { return true; },
                         steady_clock::now() - std::chrono::seconds(1)));
  EXPECT_EQ(pumps, 0);
}

}  // namespace
}  // namespace donner::tests
