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

#include <cstdlib>
#include <string>

namespace donner::tests {
namespace {

using testing::Eq;
using testing::IsEmpty;

/// Sets an environment variable for one test and restores the previous state afterwards, so the
/// cases can force the automated-lane path without depending on where they are run.
///
/// Copied from donner/gpu/baseline/FrozenBaselinePolicy_tests.cc rather than shared: PR 1095
/// (shader-validators-fail-closed-on-ci) hoists this into
/// donner/base/tests/ScopedEnvironmentVariable, but that branch has not landed on main yet. Once
/// it does, every local copy of this class, including this one, should move onto it.
class ScopedEnvironmentVariable {
public:
  ScopedEnvironmentVariable(const char* name, const char* value) : name_(name) {
    if (const char* previous = std::getenv(name); previous != nullptr) {
      previous_ = previous;
      hadPrevious_ = true;
    }
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }

  ~ScopedEnvironmentVariable() {
    if (hadPrevious_) {
      setenv(name_.c_str(), previous_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
  std::string name_;
  std::string previous_;
  bool hadPrevious_ = false;
};

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
