/// @file
/// Covers the GPU-family to texture-limit mapping from Metal's feature set tables. The mapping is
/// a pure function, so every family is checked here, including ones no test host has.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/gpu/metal/MetalDevice.h"

namespace donner::gpu::metal {
namespace {

using testing::Eq;

TEST(MetalTextureLimit, AMacFamilyDeviceAllocatesSixteenThousandTexels) {
  EXPECT_THAT(MetalDevice::MaxTextureDimension2DFor({.mac = true, .apple3OrLater = false}),
              Eq(16384u));
}

TEST(MetalTextureLimit, AnAppleFamilyThreeDeviceAllocatesSixteenThousandTexels) {
  EXPECT_THAT(MetalDevice::MaxTextureDimension2DFor({.mac = false, .apple3OrLater = true}),
              Eq(16384u));
}

TEST(MetalTextureLimit, AnEarlierAppleFamilyDeviceAllocatesEightThousandTexels) {
  EXPECT_THAT(MetalDevice::MaxTextureDimension2DFor({.mac = false, .apple3OrLater = false}),
              Eq(8192u));
}

}  // namespace
}  // namespace donner::gpu::metal
