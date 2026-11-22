/**
 * @file
 * A gtest binary linked with `//donner/base:gtest_timeout_main` whose cases stall on request.
 * It is a binary, not a test: `gtest_timeout_main_watchdog_tests.sh` runs it in a subprocess and
 * checks that the watchdog names the stalled case before the (real or simulated) Bazel timeout.
 *
 * Environment:
 * - `DONNER_HANG_FIXTURE_SPEND_MS`: how long `HangFixture.SpendsTime` runs before passing.
 * - `DONNER_HANG_FIXTURE_HANG_IN_SUITE_SETUP=1`: `HangInSuiteSetup` stalls in `SetUpTestSuite`,
 *   outside any test case.
 * - `DONNER_HANG_FIXTURE_HANG_IN_SUITE_TEARDOWN=1`: `HangInSuiteTeardown` stalls in
 *   `TearDownTestSuite`, after its last case has ended.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace donner::tests {
namespace {

bool EnvIsOne(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && std::string_view(value) == "1";
}

void StallForever() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours(1));
  }
}

TEST(HangFixture, Passes) {}

TEST(HangFixture, SpendsTime) {
  const char* spendMs = std::getenv("DONNER_HANG_FIXTURE_SPEND_MS");
  ASSERT_NE(spendMs, nullptr) << "DONNER_HANG_FIXTURE_SPEND_MS must be set";
  std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(spendMs)));
}

TEST(HangFixture, Hangs) {
  StallForever();
}

class HangInSuiteSetup : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    if (EnvIsOne("DONNER_HANG_FIXTURE_HANG_IN_SUITE_SETUP")) {
      StallForever();
    }
  }
};

TEST_F(HangInSuiteSetup, Runs) {}

class HangInSuiteTeardown : public ::testing::Test {
protected:
  static void TearDownTestSuite() {
    if (EnvIsOne("DONNER_HANG_FIXTURE_HANG_IN_SUITE_TEARDOWN")) {
      StallForever();
    }
  }
};

TEST_F(HangInSuiteTeardown, Runs) {}

}  // namespace
}  // namespace donner::tests
