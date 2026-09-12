/// @file
/// Covers the single definition of the automated-lane markers. Both
/// donner/gpu/baseline/FrozenBaselinePolicy.{h,cc} and
/// donner/base/tests/EnvironmentCapabilityGate.h delegate to these functions rather than keeping
/// their own copy, so a marker dropped here silently drops the automated-lane detection for both
/// a pixel-baseline gate and a symlink-capability gate at once. Checked directly, the same way
/// FrozenBaselinePolicy_tests.cc used to check its own local copy of this logic.

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
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), IsEmpty());
  EXPECT_FALSE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, TheHostedRunnerMarkerIsDetected) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq("GITHUB_ACTIONS"));
  EXPECT_TRUE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, TheExplicitOverrideMarkerIsDetected) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", "1");

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER"));
  EXPECT_TRUE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, AnEmptyMarkerDoesNotCountAsSet) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), IsEmpty());
  EXPECT_FALSE(RunningUnderContinuousIntegration());
}

TEST(ContinuousIntegrationMarkersTests, TheHostedRunnerMarkerIsCheckedFirst) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", "1");

  // Both markers are set; the reported one is whichever this list checks first, so a message
  // naming it stays consistent regardless of which lane variables happen to be present together.
  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq(kContinuousIntegrationMarkers[0]));
}

}  // namespace
}  // namespace donner::tests
