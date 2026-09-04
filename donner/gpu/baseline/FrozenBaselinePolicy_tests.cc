/// @file
/// Covers what a run does when its adapter has no committed pixel baseline. These cases need no
/// GPU, so the rule that keeps the pixel gate from silently retiring itself is itself checked on
/// every lane, including the ones with no device to check it on.

#include "donner/gpu/baseline/FrozenBaselinePolicy.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/base/tests/ScopedEnvironmentVariable.h"

namespace donner::gpu::baseline {
namespace {

using testing::HasSubstr;
using testing::Not;

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

TEST(FrozenBaselinePolicyTests, TheSlugIsOneDirectoryNamePerAdapter) {
  // The name the frozen pixels are filed under. Both the wgpu capture that writes those
  // directories and the per-backend slices that read them derive it from here, so a disagreement
  // would send one of them looking in a directory the other never writes.
  EXPECT_EQ(AdapterSlug("Apple M1 Pro", "Metal"), "apple_m1_pro_metal");

  // Every run of non-alphanumerics collapses to a single underscore, and no separator is left
  // dangling at either end, so one adapter resolves to one directory on every platform.
  EXPECT_EQ(AdapterSlug("llvmpipe (LLVM 21.1.7, 128 bits)", "Vulkan"),
            "llvmpipe_llvm_21_1_7_128_bits_vulkan");
  EXPECT_EQ(AdapterSlug("  spaced  out  ", "Metal"), "spaced_out_metal");

  // A driver reporting nothing usable still resolves to a name, so the caller reports a missing
  // directory rather than building a path with an empty component in it.
  EXPECT_EQ(AdapterSlug("", ""), "unknown_adapter");
  EXPECT_EQ(AdapterSlug("///", "---"), "unknown_adapter");
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
