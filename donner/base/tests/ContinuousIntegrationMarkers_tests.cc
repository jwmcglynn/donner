/// @file
/// Covers the single definition of the automated-lane markers and the skip-or-fail decision made
/// from them. The environment-capability, GPU device and external-tool gates all call these rather
/// than keeping their own copy, so a marker dropped here silently drops automated-lane detection
/// for every one of them at once.

#include "donner/base/tests/ContinuousIntegrationMarkers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ScopedEnvironmentVariable.h"

namespace donner::tests {
namespace {

using testing::Eq;
using testing::IsEmpty;

TEST(ContinuousIntegrationMarkersTests, NoMarkerMeansNoAutomatedLane) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_AUTOMATED_LANE", nullptr);

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), IsEmpty());
  EXPECT_FALSE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, TheHostedRunnerMarkerIsDetected) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_AUTOMATED_LANE", nullptr);

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq("GITHUB_ACTIONS"));
  EXPECT_TRUE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, TheExplicitOverrideMarkerIsDetected) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_AUTOMATED_LANE", "1");

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq("DONNER_AUTOMATED_LANE"));
  EXPECT_TRUE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, AnEmptyMarkerDoesNotCountAsSet) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "");
  const ScopedEnvironmentVariable donnerOverride("DONNER_AUTOMATED_LANE", nullptr);

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), IsEmpty());
  EXPECT_FALSE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, TheHostedRunnerMarkerIsCheckedFirst) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_AUTOMATED_LANE", "1");

  // Both markers are set; the reported one is whichever this list checks first, so a message
  // naming it stays consistent regardless of which lane variables happen to be present together.
  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq(kContinuousIntegrationMarkers[0]));
}

TEST(ContinuousIntegrationMarkersTests, AnAutomatedLaneFailsWhereADeveloperMachineSkips) {
  EXPECT_THAT(DispositionForMissingRequirement(/*underContinuousIntegration=*/true),
              Eq(MissingRequirementDisposition::FailClosed));
  EXPECT_THAT(DispositionForMissingRequirement(/*underContinuousIntegration=*/false),
              Eq(MissingRequirementDisposition::Skip));
}

TEST(ContinuousIntegrationMarkersTests, DispositionsPrintTheirNames) {
  EXPECT_THAT(testing::PrintToString(MissingRequirementDisposition::Skip), Eq("Skip"));
  EXPECT_THAT(testing::PrintToString(MissingRequirementDisposition::FailClosed), Eq("FailClosed"));
}

}  // namespace
}  // namespace donner::tests
