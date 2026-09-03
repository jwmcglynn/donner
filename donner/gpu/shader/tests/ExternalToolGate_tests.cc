/// @file
/// Covers what a suite that drives an external validator does when the validator is not installed.
/// The rule is checked here rather than only through the suites it gates, because a machine that
/// has the tool never reaches it.

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

/// Invokes the gate the way a test case does, then records that control continued past it.
/// @param unavailableReason What the probe found, empty when the tool is usable.
/// @param reachedEnd Set true when the gate let the case continue.
void RunGate(const std::string& unavailableReason, bool* reachedEnd) {
  DONNER_REQUIRE_EXTERNAL_TOOL("spirv-val (SPIRV-Tools)", unavailableReason);
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
  const std::string message =
      MissingExternalToolMessage("spirv-val (SPIRV-Tools)", "not found on PATH", "",
                                 baseline::MissingComparisonDisposition::Skip);

  EXPECT_THAT(message, HasSubstr("spirv-val (SPIRV-Tools)"));
  EXPECT_THAT(message, HasSubstr("not found on PATH"));
  EXPECT_THAT(message, Not(HasSubstr("Failing rather than skipping")));
}

TEST(ExternalToolGateTests, TheFailureMessageNamesTheToolTheLaneAndTheRemedy) {
  const std::string message =
      MissingExternalToolMessage("spirv-val (SPIRV-Tools)", "not found on PATH", "GITHUB_ACTIONS",
                                 baseline::MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("spirv-val (SPIRV-Tools)"));
  EXPECT_THAT(message, HasSubstr("not found on PATH"));
  EXPECT_THAT(message, HasSubstr("GITHUB_ACTIONS"));
  EXPECT_THAT(message, HasSubstr("Failing rather than skipping"));
  EXPECT_THAT(message, HasSubstr("exclude the target from it explicitly"));
}

TEST(ExternalToolGateTests, AFailureWithNoMarkerStillExplainsItself) {
  const std::string message =
      MissingExternalToolMessage("the offline Metal compiler", "missing Metal Toolchain", "",
                                 baseline::MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("the offline Metal compiler"));
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

  EXPECT_NONFATAL_FAILURE(RunGate("not found on PATH", &reachedEnd),
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
    RunGate("not found on PATH", &reachedEnd);
  }

  EXPECT_FALSE(reachedEnd);
  ASSERT_THAT(results.size(), testing::Eq(1));
  EXPECT_THAT(results.GetTestPartResult(0).type(), testing::Eq(testing::TestPartResult::kSkip));
  EXPECT_THAT(results.GetTestPartResult(0).message(), HasSubstr("spirv-val (SPIRV-Tools)"));
  EXPECT_THAT(results.GetTestPartResult(0).message(), Not(HasSubstr("Failing rather than")));
}

}  // namespace
}  // namespace donner::gpu::shader
