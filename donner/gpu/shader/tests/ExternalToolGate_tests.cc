/// @file
/// Covers what a suite that drives an external tool does when that tool is not installed. The rule
/// is checked here rather than only through the suite it gates, because a machine that has the
/// tool never reaches it.

#include "donner/gpu/shader/tests/ExternalToolGate.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/base/tests/ScopedEnvironmentVariable.h"

namespace donner::gpu::shader {
namespace {

using testing::HasSubstr;
using testing::Not;

/// The only tool the gate still guards: a platform compiler that cannot be built from source here.
const std::string kToolName = "the offline Metal compiler";

/// Invokes the gate the way a test case does, then records that control continued past it.
/// @param unavailableReason What the probe found, empty when the tool is usable.
/// @param reachedEnd Set true when the gate let the case continue.
void RunGate(const std::string& unavailableReason, bool* reachedEnd) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kToolName, unavailableReason);
  *reachedEnd = true;
}

TEST(ExternalToolGateTests, AnAutomatedLaneFailsInsteadOfSkipping) {
  EXPECT_THAT(DispositionForMissingExternalTool(/*underContinuousIntegration=*/true),
              testing::Eq(baseline::MissingComparisonDisposition::FailClosed));
}

TEST(ExternalToolGateTests, ADeveloperMachineKeepsTheSkipConvenience) {
  EXPECT_THAT(DispositionForMissingExternalTool(/*underContinuousIntegration=*/false),
              testing::Eq(baseline::MissingComparisonDisposition::Skip));
}

TEST(ExternalToolGateTests, TheSkipMessageNamesTheToolAndWhatTheProbeFound) {
  const std::string message = MissingExternalToolMessage(
      kToolName, "missing Metal Toolchain", "", baseline::MissingComparisonDisposition::Skip);

  EXPECT_THAT(message, HasSubstr(kToolName));
  EXPECT_THAT(message, HasSubstr("missing Metal Toolchain"));
  EXPECT_THAT(message, Not(HasSubstr("Failing rather than skipping")));
}

TEST(ExternalToolGateTests, TheFailureMessageNamesTheToolTheLaneAndTheRemedy) {
  const std::string message =
      MissingExternalToolMessage(kToolName, "missing Metal Toolchain", "GITHUB_ACTIONS",
                                 baseline::MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr(kToolName));
  EXPECT_THAT(message, HasSubstr("missing Metal Toolchain"));
  EXPECT_THAT(message, HasSubstr("GITHUB_ACTIONS"));
  EXPECT_THAT(message, HasSubstr("Failing rather than skipping"));
  EXPECT_THAT(message, HasSubstr("exclude the target from it explicitly"));
}

TEST(ExternalToolGateTests, AFailureWithNoMarkerStillExplainsItself) {
  const std::string message = MissingExternalToolMessage(
      kToolName, "missing Metal Toolchain", "", baseline::MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr(kToolName));
  EXPECT_THAT(message, HasSubstr("Failing rather than skipping"));
  EXPECT_THAT(message, Not(HasSubstr("Automated lane identified by")));
}

TEST(ExternalToolGateTests, AUsableToolLetsTheCaseRun) {
  bool reachedEnd = false;
  RunGate("", &reachedEnd);

  EXPECT_TRUE(reachedEnd);
}

TEST(ExternalToolGateTests, AnAutomatedLaneFailsAndStopsTheCase) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");

  // EXPECT_NONFATAL_FAILURE's statement cannot name a non-static local.
  static bool reachedEnd;
  reachedEnd = false;

  EXPECT_NONFATAL_FAILURE(RunGate("missing Metal Toolchain", &reachedEnd),
                          "Failing rather than skipping");
  EXPECT_FALSE(reachedEnd);
}

TEST(ExternalToolGateTests, ADeveloperMachineSkipsAndStopsTheCase) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  bool reachedEnd = false;
  // Intercepted rather than allowed to land, so this case reports its assertions instead of
  // reporting itself as skipped.
  testing::TestPartResultArray results;
  {
    const testing::ScopedFakeTestPartResultReporter reporter(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &results);
    RunGate("missing Metal Toolchain", &reachedEnd);
  }

  EXPECT_FALSE(reachedEnd);
  ASSERT_THAT(results.size(), testing::Eq(1));
  EXPECT_THAT(results.GetTestPartResult(0).type(), testing::Eq(testing::TestPartResult::kSkip));
  EXPECT_THAT(results.GetTestPartResult(0).message(), HasSubstr(kToolName));
  EXPECT_THAT(results.GetTestPartResult(0).message(), Not(HasSubstr("Failing rather than")));
}

}  // namespace
}  // namespace donner::gpu::shader
