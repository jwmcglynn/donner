/// @file
/// Covers what a run does when its adapter has no committed pixel baseline. These cases need no
/// GPU, so the rule that keeps the pixel gate from silently retiring itself is itself checked on
/// every lane, including the ones with no device to check it on.

#include "donner/gpu/baseline/FrozenBaselinePolicy.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace donner::gpu::baseline {
namespace {

using testing::HasSubstr;
using testing::Not;

/// Sets an environment variable for one test and restores the previous state afterwards, so the
/// cases can force the automated-lane path without depending on where they are run.
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

TEST(FrozenBaselinePolicyTests, AnAutomatedLaneFailsInsteadOfSkipping) {
  EXPECT_THAT(DispositionForUnbaselinedAdapter(/*underContinuousIntegration=*/true),
              testing::Eq(MissingComparisonDisposition::FailClosed));
}

TEST(FrozenBaselinePolicyTests, ADeveloperMachineKeepsTheSkipConvenience) {
  EXPECT_THAT(DispositionForUnbaselinedAdapter(/*underContinuousIntegration=*/false),
              testing::Eq(MissingComparisonDisposition::Skip));
}

TEST(FrozenBaselinePolicyTests, TheHostedRunnerMarkerSelectsTheFailClosedDisposition) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_TRUE(RunningUnderContinuousIntegration());
  EXPECT_THAT(DispositionForUnbaselinedAdapter(RunningUnderContinuousIntegration()),
              testing::Eq(MissingComparisonDisposition::FailClosed));
}

TEST(FrozenBaselinePolicyTests, TheExplicitOverrideSelectsTheFailClosedDisposition) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", "1");

  EXPECT_TRUE(RunningUnderContinuousIntegration());
}

TEST(FrozenBaselinePolicyTests, NoMarkerMeansNoAutomatedLane) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_FALSE(RunningUnderContinuousIntegration());
  EXPECT_THAT(DispositionForUnbaselinedAdapter(RunningUnderContinuousIntegration()),
              testing::Eq(MissingComparisonDisposition::Skip));
}

TEST(FrozenBaselinePolicyTests, AnEmptyMarkerDoesNotCountAsAnAutomatedLane) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_FALSE(RunningUnderContinuousIntegration());
}

TEST(FrozenBaselinePolicyTests, AnAutomatedLaneWithNoDeviceFailsInsteadOfSkipping) {
  EXPECT_THAT(DispositionForMissingAdapter(/*underContinuousIntegration=*/true),
              testing::Eq(MissingComparisonDisposition::FailClosed));
}

TEST(FrozenBaselinePolicyTests, ADeveloperMachineWithNoDeviceStillSkips) {
  EXPECT_THAT(DispositionForMissingAdapter(/*underContinuousIntegration=*/false),
              testing::Eq(MissingComparisonDisposition::Skip));
}

TEST(FrozenBaselinePolicyTests, TheMissingDeviceRuleFollowsTheSameMarkersAsTheMissingBaselineRule) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  EXPECT_THAT(DispositionForMissingAdapter(RunningUnderContinuousIntegration()),
              testing::Eq(DispositionForUnbaselinedAdapter(RunningUnderContinuousIntegration())));
}

TEST(FrozenBaselinePolicyTests, TheNoDeviceMessageNamesTheGate) {
  const std::string message =
      NoAdapterMessage("the frozen pixel check", MissingComparisonDisposition::Skip);

  EXPECT_THAT(message, HasSubstr("the frozen pixel check"));
  EXPECT_THAT(message, Not(HasSubstr("Failing rather than skipping")));
}

TEST(FrozenBaselinePolicyTests, TheFailingNoDeviceMessageExplainsWhySkippingWasNotAnOption) {
  const std::string message =
      NoAdapterMessage("the frozen pixel check", MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("the frozen pixel check"));
  EXPECT_THAT(message, HasSubstr("disabled the gate"));
}

TEST(FrozenBaselinePolicyTests, TheMessageNamesTheDirectoryToCommit) {
  const std::string message = UnbaselinedAdapterMessage("Example GPU", "Metal", "example_gpu_metal",
                                                        "/outputs/example_gpu_metal", "",
                                                        MissingComparisonDisposition::Skip);

  EXPECT_THAT(message, HasSubstr("Example GPU"));
  EXPECT_THAT(message, HasSubstr("Metal"));
  EXPECT_THAT(message, HasSubstr("/outputs/example_gpu_metal"));
  EXPECT_THAT(message, HasSubstr("donner/gpu/baseline/baselines/example_gpu_metal/"));
}

TEST(FrozenBaselinePolicyTests, TheFailingMessageExplainsWhySkippingWasNotAnOption) {
  const std::string message = UnbaselinedAdapterMessage("Example GPU", "Metal", "example_gpu_metal",
                                                        "/outputs/example_gpu_metal", "",
                                                        MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("donner/gpu/baseline/baselines/example_gpu_metal/"));
  EXPECT_THAT(message, HasSubstr("silently stop running"));
}

TEST(FrozenBaselinePolicyTests, AFailedCaptureIsReportedInsteadOfAMissingPath) {
  const std::string message = UnbaselinedAdapterMessage("Example GPU", "Metal", "example_gpu_metal",
                                                        "", "device lost while capturing",
                                                        MissingComparisonDisposition::FailClosed);

  EXPECT_THAT(message, HasSubstr("device lost while capturing"));
}

}  // namespace
}  // namespace donner::gpu::baseline
