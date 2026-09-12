#include "donner/gpu/metal/tests/MetalValidationProfile.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace donner::gpu::metal::tests {
namespace {

MetalValidationObservation KnownParavirtualObservation() {
  return MetalValidationObservation{
      .gpuName = "Apple Paravirtual device",
      .osMajor = 26,
      .osMinor = 6,
      .osPatch = 2,
      .osBuild = "25G83",
      .shaderValidationState = 2,
  };
}

TEST(MetalValidationProfileTest, ExactDisabledObservationSelectsTextureUsageException) {
  EXPECT_THAT(ClassifyMetalValidationProfile(KnownParavirtualObservation()),
              testing::Eq(MetalValidationProfile::ParavirtualTextureUsageOff));
}

TEST(MetalValidationProfileTest, EnabledStateSelectsFullForEveryDeviceAndOs) {
  MetalValidationObservation unknown;
  unknown.shaderValidationState = 1;
  EXPECT_THAT(ClassifyMetalValidationProfile(unknown), testing::Eq(MetalValidationProfile::Full));
  MetalValidationObservation known = KnownParavirtualObservation();
  known.shaderValidationState = 1;
  EXPECT_THAT(ClassifyMetalValidationProfile(known), testing::Eq(MetalValidationProfile::Full));
}

TEST(MetalValidationProfileTest, DisabledStateRequiresExactDeviceName) {
  for (const char* name : {"", "Other device", "Apple Paravirtual", "apple Paravirtual device",
                           "Apple Paravirtual device extra", "extra Apple Paravirtual device"}) {
    SCOPED_TRACE(name);
    MetalValidationObservation observation = KnownParavirtualObservation();
    observation.gpuName = name;
    EXPECT_THAT(ClassifyMetalValidationProfile(observation),
                testing::Eq(MetalValidationProfile::Unsupported));
  }
}

TEST(MetalValidationProfileTest, DisabledStateRequiresExactOsVersion) {
  for (const std::array<int, 3>& version :
       {std::array{25, 6, 2}, std::array{27, 6, 2}, std::array{26, 5, 2}, std::array{26, 7, 2},
        std::array{26, 6, 1}, std::array{26, 6, 3}}) {
    SCOPED_TRACE(testing::PrintToString(version));
    MetalValidationObservation observation = KnownParavirtualObservation();
    observation.osMajor = version[0];
    observation.osMinor = version[1];
    observation.osPatch = version[2];
    EXPECT_THAT(ClassifyMetalValidationProfile(observation),
                testing::Eq(MetalValidationProfile::Unsupported));
  }
}

TEST(MetalValidationProfileTest, DisabledStateRequiresExactOsBuild) {
  for (const char* build : {"", "25G82", "25G84", "25g83", "25G83 extra", "extra 25G83"}) {
    SCOPED_TRACE(build);
    MetalValidationObservation observation = KnownParavirtualObservation();
    observation.osBuild = build;
    EXPECT_THAT(ClassifyMetalValidationProfile(observation),
                testing::Eq(MetalValidationProfile::Unsupported));
  }
}

TEST(MetalValidationProfileTest, DefaultAndUnknownStatesNeverSelectAProfile) {
  for (const std::int64_t state : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{3},
                                   std::numeric_limits<std::int64_t>::max()}) {
    SCOPED_TRACE(state);
    MetalValidationObservation observation = KnownParavirtualObservation();
    observation.shaderValidationState = state;
    EXPECT_THAT(ClassifyMetalValidationProfile(observation),
                testing::Eq(MetalValidationProfile::Unsupported));
  }
}

}  // namespace
}  // namespace donner::gpu::metal::tests
