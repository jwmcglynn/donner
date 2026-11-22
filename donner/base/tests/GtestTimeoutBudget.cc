#include "donner/base/tests/GtestTimeoutBudget.h"

#include <algorithm>
#include <cstdint>

namespace donner::tests {

using std::chrono::milliseconds;

std::optional<milliseconds> ParsePositiveSeconds(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  const std::int64_t maxSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(kMaxParsedSeconds).count();
  std::int64_t seconds = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    // Saturate instead of overflowing on arbitrarily long digit strings.
    seconds = std::min(seconds * 10 + (c - '0'), maxSeconds + 1);
  }

  if (seconds == 0) {
    return std::nullopt;
  }
  return std::min(milliseconds(std::chrono::seconds(seconds)), kMaxParsedSeconds);
}

milliseconds WatchdogMargin(milliseconds testTimeout) {
  if (testTimeout < 2 * kMinWatchdogMargin) {
    return testTimeout / 2;
  }
  return std::clamp(testTimeout / 10, kMinWatchdogMargin, kMaxWatchdogMargin);
}

std::optional<milliseconds> WatchdogDeadline(const char* testTimeoutEnv) {
  if (testTimeoutEnv == nullptr) {
    return std::nullopt;
  }
  const std::optional<milliseconds> testTimeout = ParsePositiveSeconds(testTimeoutEnv);
  if (!testTimeout) {
    return std::nullopt;
  }
  return *testTimeout - WatchdogMargin(*testTimeout);
}

milliseconds WatchdogAlarmInterval(std::optional<milliseconds> perCaseBudget,
                                   std::optional<milliseconds> deadline, milliseconds elapsed) {
  if (!deadline) {
    return perCaseBudget.value_or(milliseconds(0));
  }

  const milliseconds remaining = std::max(*deadline - elapsed, milliseconds(1));
  return perCaseBudget ? std::min(*perCaseBudget, remaining) : remaining;
}

}  // namespace donner::tests
