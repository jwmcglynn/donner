/// @file
/// Covers what DONNER_REQUIRE_METAL_DEVICE does when a fixture's device is null. The macro's
/// missing-device branches are exercised only when a runner actually loses its Metal device; every
/// case on a working lane takes the non-null path, so a regression in the macro itself would
/// otherwise reach production only when a runner failed. Checked here directly, with no device
/// needed, the same way ExternalToolGate_tests.cc checks DONNER_REQUIRE_EXTERNAL_TOOL.

#include "donner/gpu/metal/tests/MetalDeviceGate.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ScopedEnvironmentVariable.h"

namespace donner::gpu::metal::tests {
namespace {

using testing::HasSubstr;
using testing::Not;

/// Invokes the macro the way a fixture's SetUp() does, then records that control continued past
/// it.
/// @param device Pointer-like value the macro gates on; null exercises the missing-device path.
/// @param reachedEnd Set true when the macro let the case continue.
void RunGate(const void* device, bool* reachedEnd) {
  DONNER_REQUIRE_METAL_DEVICE(device, "the fixture under test");
  *reachedEnd = true;
}

TEST(MetalDeviceGateTests, AnAvailableDeviceLetsTheCaseRun) {
  const int fakeDevice = 0;
  bool reachedEnd = false;
  RunGate(&fakeDevice, &reachedEnd);

  EXPECT_TRUE(reachedEnd);
}

TEST(MetalDeviceGateTests, AnAutomatedLaneFailsAndStopsTheCase) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", "true");

  // EXPECT_FATAL_FAILURE's statement cannot name a non-static local.
  static bool reachedEnd;
  reachedEnd = false;

  EXPECT_FATAL_FAILURE(RunGate(nullptr, &reachedEnd), "Failing rather than skipping");
  EXPECT_FALSE(reachedEnd);
}

TEST(MetalDeviceGateTests, ADeveloperMachineSkipsAndStopsTheCase) {
  const ScopedEnvironmentVariable githubActions("GITHUB_ACTIONS", nullptr);
  const ScopedEnvironmentVariable donnerOverride("DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER", nullptr);

  bool reachedEnd = false;
  // Intercepted rather than allowed to land, so this case reports its assertions instead of
  // reporting itself as skipped.
  testing::TestPartResultArray results;
  {
    const testing::ScopedFakeTestPartResultReporter reporter(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &results);
    RunGate(nullptr, &reachedEnd);
  }

  EXPECT_FALSE(reachedEnd);
  ASSERT_THAT(results.size(), testing::Eq(1));
  EXPECT_THAT(results.GetTestPartResult(0).type(), testing::Eq(testing::TestPartResult::kSkip));
  EXPECT_THAT(results.GetTestPartResult(0).message(), HasSubstr("the fixture under test"));
  EXPECT_THAT(results.GetTestPartResult(0).message(), Not(HasSubstr("Failing rather than")));
}

}  // namespace
}  // namespace donner::gpu::metal::tests
