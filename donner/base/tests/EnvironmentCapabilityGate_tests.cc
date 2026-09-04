/// @file
/// Covers what DONNER_REQUIRE_ENVIRONMENT_CAPABILITY does when the capability it gates is
/// unavailable. The macro's missing-capability branches are exercised only when a lane actually
/// lacks the capability (a sandbox that forbids symlink creation, say); every case on a working
/// lane takes the available path, so a regression in the macro itself would otherwise reach
/// production only when a lane lost the capability. Checked here directly, with no such lane
/// needed, the same way MetalDeviceGate_tests.cc checks DONNER_REQUIRE_METAL_DEVICE.

#include "donner/base/tests/EnvironmentCapabilityGate.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace donner::tests {
namespace {

using testing::Eq;
using testing::HasSubstr;
using testing::Not;

/// Sets an environment variable for one test and restores the previous state afterwards, so the
/// cases can force the automated-lane path without depending on where they are run.
///
/// Copied from donner/gpu/baseline/FrozenBaselinePolicy_tests.cc rather than shared: PR 1095
/// (shader-validators-fail-closed-on-ci) hoists this into
/// donner/base/tests/ScopedEnvironmentVariable, but that branch has not landed on main yet. Once
/// it does, this copy should move onto the shared helper instead of a third one existing.
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

/// Invokes the macro the way a test case does, then records that control continued past it.
/// @param unavailableReason Reason string the macro gates on; non-empty exercises the
///   missing-capability path.
/// @param reachedEnd Set true when the macro let the case continue.
void RunGate(const std::string& unavailableReason, bool* reachedEnd) {
  DONNER_REQUIRE_ENVIRONMENT_CAPABILITY(unavailableReason, "the capability under test");
  *reachedEnd = true;
}

TEST(EnvironmentCapabilityGateTests, AnAvailableCapabilityLetsTheCaseRun) {
  bool reachedEnd = false;
  RunGate(/*unavailableReason=*/"", &reachedEnd);

  EXPECT_TRUE(reachedEnd);
}

TEST(EnvironmentCapabilityGateTests, AnAutomatedLaneFailsAndStopsTheCase) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");

  // EXPECT_FATAL_FAILURE's statement cannot name a non-static local.
  static bool reachedEnd;
  reachedEnd = false;

  EXPECT_FATAL_FAILURE(RunGate("permission denied", &reachedEnd), "Failing rather than skipping");
  EXPECT_FALSE(reachedEnd);
}

TEST(EnvironmentCapabilityGateTests, ADeveloperMachineSkipsAndStopsTheCase) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  bool reachedEnd = false;
  // Intercepted rather than allowed to land, so this case reports its assertions instead of
  // reporting itself as skipped. This is the check that would catch a mutant collapsing the
  // automated-lane branch onto this one: it fails unless the intercepted result is exactly one
  // kSkip part, so a mutant that skips under the marker too would flip
  // AnAutomatedLaneFailsAndStopsTheCase instead, and a mutant that fails here too would flip this
  // one.
  testing::TestPartResultArray results;
  {
    const testing::ScopedFakeTestPartResultReporter reporter(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &results);
    RunGate("permission denied", &reachedEnd);
  }

  EXPECT_FALSE(reachedEnd);
  ASSERT_THAT(results.size(), Eq(1));
  EXPECT_THAT(results.GetTestPartResult(0).type(), Eq(testing::TestPartResult::kSkip));
  EXPECT_THAT(results.GetTestPartResult(0).message(), HasSubstr("the capability under test"));
  EXPECT_THAT(results.GetTestPartResult(0).message(), HasSubstr("permission denied"));
  EXPECT_THAT(results.GetTestPartResult(0).message(), Not(HasSubstr("Failing rather than")));
}

TEST(EnvironmentCapabilityGateTests, TheHostedRunnerMarkerSelectsTheFailClosedDisposition) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_TRUE(RunningUnderContinuousIntegration());
  EXPECT_THAT(DispositionForMissingEnvironmentCapability(RunningUnderContinuousIntegration()),
              Eq(MissingEnvironmentCapabilityDisposition::FailClosed));
  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq("GITHUB_ACTIONS"));
}

TEST(EnvironmentCapabilityGateTests, TheExplicitOverrideSelectsTheFailClosedDisposition) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", "1");

  EXPECT_TRUE(RunningUnderContinuousIntegration());
  EXPECT_THAT(FirstContinuousIntegrationMarkerSet(), Eq("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER"));
}

TEST(EnvironmentCapabilityGateTests, NoMarkerMeansNoAutomatedLane) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_FALSE(RunningUnderContinuousIntegration());
  EXPECT_THAT(DispositionForMissingEnvironmentCapability(RunningUnderContinuousIntegration()),
              Eq(MissingEnvironmentCapabilityDisposition::Skip));
  EXPECT_TRUE(FirstContinuousIntegrationMarkerSet().empty());
}

TEST(EnvironmentCapabilityGateTests, AnEmptyMarkerDoesNotCountAsAnAutomatedLane) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_FALSE(RunningUnderContinuousIntegration());
}

TEST(EnvironmentCapabilityGateTests, TheMessageNamesTheCapability) {
  const std::string message = MissingEnvironmentCapabilityMessage(
      "the ability to create filesystem symlinks", "permission denied", "",
      MissingEnvironmentCapabilityDisposition::Skip);

  EXPECT_THAT(message, HasSubstr("the ability to create filesystem symlinks"));
  EXPECT_THAT(message, HasSubstr("permission denied"));
  EXPECT_THAT(message, Not(HasSubstr("Failing rather than skipping")));
}

TEST(EnvironmentCapabilityGateTests,
     TheFailingMessageExplainsWhySkippingWasNotAnOptionAndNamesTheMarker) {
  const std::string message = MissingEnvironmentCapabilityMessage(
      "the ability to create filesystem symlinks", "permission denied", "GITHUB_ACTIONS",
      MissingEnvironmentCapabilityDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("the ability to create filesystem symlinks"));
  EXPECT_THAT(message, HasSubstr("Failing rather than skipping"));
  EXPECT_THAT(message, HasSubstr("Automated lane identified by GITHUB_ACTIONS"));
}

TEST(EnvironmentCapabilityGateTests, TheFailingMessageOmitsTheMarkerLineWhenThereIsNone) {
  const std::string message = MissingEnvironmentCapabilityMessage(
      "the ability to create filesystem symlinks", "permission denied", "",
      MissingEnvironmentCapabilityDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("Failing rather than skipping"));
  EXPECT_THAT(message, Not(HasSubstr("Automated lane identified by")));
}

}  // namespace
}  // namespace donner::tests
