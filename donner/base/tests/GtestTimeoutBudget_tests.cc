#include "donner/base/tests/GtestTimeoutBudget.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace donner::tests {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::Le;
using testing::Lt;
using testing::Optional;

TEST(GtestTimeoutBudgetTest, ParsePositiveSecondsAcceptsDecimalSeconds) {
  EXPECT_THAT(ParsePositiveSeconds("1"), Optional(Eq(milliseconds(seconds(1)))));
  EXPECT_THAT(ParsePositiveSeconds("60"), Optional(Eq(milliseconds(seconds(60)))));
  EXPECT_THAT(ParsePositiveSeconds("0900"), Optional(Eq(milliseconds(seconds(900)))));
}

TEST(GtestTimeoutBudgetTest, ParsePositiveSecondsRejectsMalformedAndZero) {
  for (const char* value : {"", "0", "000", "-5", "+5", " 5", "5 ", "5s", "1.5", "abc", "0x10"}) {
    EXPECT_THAT(ParsePositiveSeconds(value), Eq(std::nullopt)) << "value: '" << value << "'";
  }
}

TEST(GtestTimeoutBudgetTest, ParsePositiveSecondsClampsHugeValues) {
  EXPECT_THAT(ParsePositiveSeconds("2592000"), Optional(Eq(kMaxParsedSeconds)));
  EXPECT_THAT(ParsePositiveSeconds("2592001"), Optional(Eq(kMaxParsedSeconds)));
  EXPECT_THAT(ParsePositiveSeconds("99999999999999999999999999999999999999"),
              Optional(Eq(kMaxParsedSeconds)));
}

TEST(GtestTimeoutBudgetTest, WatchdogDeadlineIsAbsentWithoutUsableTestTimeout) {
  EXPECT_THAT(WatchdogDeadline(nullptr), Eq(std::nullopt));
  EXPECT_THAT(WatchdogDeadline(""), Eq(std::nullopt));
  EXPECT_THAT(WatchdogDeadline("0"), Eq(std::nullopt));
  EXPECT_THAT(WatchdogDeadline("sixty"), Eq(std::nullopt));
}

TEST(GtestTimeoutBudgetTest, WatchdogDeadlineForBazelSizes) {
  // small, medium, large/long and enormous/eternal default timeouts.
  EXPECT_THAT(WatchdogDeadline("60"), Optional(Eq(milliseconds(seconds(54)))));
  EXPECT_THAT(WatchdogDeadline("300"), Optional(Eq(milliseconds(seconds(270)))));
  EXPECT_THAT(WatchdogDeadline("900"), Optional(Eq(milliseconds(seconds(840)))));
  EXPECT_THAT(WatchdogDeadline("3600"), Optional(Eq(milliseconds(seconds(3540)))));
}

TEST(GtestTimeoutBudgetTest, WatchdogDeadlineForShortAndHugeTimeouts) {
  EXPECT_THAT(WatchdogDeadline("1"), Optional(Eq(milliseconds(500))));
  EXPECT_THAT(WatchdogDeadline("3"), Optional(Eq(milliseconds(1500))));
  EXPECT_THAT(WatchdogDeadline("9"), Optional(Eq(milliseconds(4500))));
  EXPECT_THAT(WatchdogDeadline("10"), Optional(Eq(milliseconds(seconds(5)))));
  EXPECT_THAT(WatchdogDeadline("12"), Optional(Eq(milliseconds(seconds(7)))));
  EXPECT_THAT(WatchdogDeadline("99999999999"),
              Optional(Eq(kMaxParsedSeconds - kMaxWatchdogMargin)));
}

TEST(GtestTimeoutBudgetTest, WatchdogMarginAlwaysLeavesTimeOnBothSides) {
  milliseconds previousDeadline(0);
  for (int timeoutSeconds = 1; timeoutSeconds <= 7200; ++timeoutSeconds) {
    const milliseconds timeout = seconds(timeoutSeconds);
    const milliseconds margin = WatchdogMargin(timeout);
    const milliseconds deadline = timeout - margin;
    ASSERT_THAT(margin, Lt(timeout)) << "TEST_TIMEOUT=" << timeoutSeconds;
    ASSERT_THAT(margin, Gt(milliseconds(0))) << "TEST_TIMEOUT=" << timeoutSeconds;
    ASSERT_THAT(margin, Le(kMaxWatchdogMargin)) << "TEST_TIMEOUT=" << timeoutSeconds;
    // A longer timeout never yields an earlier deadline.
    ASSERT_THAT(deadline, Ge(previousDeadline)) << "TEST_TIMEOUT=" << timeoutSeconds;
    previousDeadline = deadline;
  }
}

TEST(GtestTimeoutBudgetTest, AlarmUsesPerCaseBudgetWithoutDeadline) {
  EXPECT_THAT(WatchdogAlarmInterval(seconds(60), std::nullopt, seconds(500)), Eq(seconds(60)));
  EXPECT_THAT(WatchdogAlarmInterval(std::nullopt, std::nullopt, seconds(5)), Eq(milliseconds(0)));
}

TEST(GtestTimeoutBudgetTest, AlarmIsCappedByTimeLeftBeforeDeadline) {
  // A small target: deadline 54 s. A case starting at 0 s gets 54 s, not its 60 s budget.
  EXPECT_THAT(WatchdogAlarmInterval(seconds(60), seconds(54), seconds(0)), Eq(seconds(54)));
  // Earlier cases spent 30 s, so a stalled case gets only the remaining 24 s.
  EXPECT_THAT(WatchdogAlarmInterval(seconds(60), seconds(54), seconds(30)), Eq(seconds(24)));
  // A shorter per-case budget still wins.
  EXPECT_THAT(WatchdogAlarmInterval(seconds(20), seconds(54), seconds(0)), Eq(seconds(20)));
  // Between cases only the deadline applies.
  EXPECT_THAT(WatchdogAlarmInterval(std::nullopt, seconds(840), seconds(40)), Eq(seconds(800)));
}

TEST(GtestTimeoutBudgetTest, AlarmFiresAlmostImmediatelyOncePastDeadline) {
  EXPECT_THAT(WatchdogAlarmInterval(seconds(60), seconds(54), seconds(54)), Eq(milliseconds(1)));
  EXPECT_THAT(WatchdogAlarmInterval(std::nullopt, seconds(54), seconds(70)), Eq(milliseconds(1)));
}

}  // namespace
}  // namespace donner::tests
