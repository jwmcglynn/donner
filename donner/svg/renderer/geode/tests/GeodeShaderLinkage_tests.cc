/// @file
/// A native production build links only the platform-native shader projections. Without the
/// test-only WGSL alternates, a device that consumes WGSL receives nothing from any production
/// creator and module creation refuses it, while a native device still receives its projection.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <string_view>

#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/tests/ProjectionCapturingDevice.h"

namespace donner::geode {
namespace {

using testing::AllOf;
using testing::Field;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::Not;
using tests::ProjectionCapturingDevice;

/// The projection a native device on this platform consumes.
constexpr gpu::ShaderSourceKind kPlatformNativeKind =
#if defined(__APPLE__)
    gpu::ShaderSourceKind::Msl;
#else
    gpu::ShaderSourceKind::Spirv;
#endif

/// Matches a descriptor that carries a non-empty projection of the platform's native kind.
MATCHER(CarriesANativeProjection, "carries a non-empty platform-native projection") {
  if constexpr (kPlatformNativeKind == gpu::ShaderSourceKind::Msl) {
    return ExplainMatchResult(
        AllOf(Field("sourceKind", &gpu::ShaderModuleDescriptor::sourceKind, kPlatformNativeKind),
              Field("sourceText", &gpu::ShaderModuleDescriptor::sourceText, Not(IsEmpty()))),
        arg, result_listener);
  } else {
    return ExplainMatchResult(
        AllOf(Field("sourceKind", &gpu::ShaderModuleDescriptor::sourceKind, kPlatformNativeKind),
              Field("spirvWords", &gpu::ShaderModuleDescriptor::spirvWords, Not(IsEmpty()))),
        arg, result_listener);
  }
}

/// One production module creator.
struct CreatorCase {
  std::string_view name;                                   //!< Test case name.
  gpu::Result<gpu::ShaderModule> (*create)(gpu::Device&);  //!< Module creator under test.
};

/// Prints the creator name. @param os Output stream. @param creator Case to print.
std::ostream& operator<<(std::ostream& os, const CreatorCase& creator) {
  return os << creator.name;
}

/// Every family whose module the Geode renderer creates through `GeodeShaders.h`.
const CreatorCase kCreators[] = {
    {"SlugFill", &createSlugFillShader},
    {"SlugGradient", &createSlugGradientShader},
    {"SlugMask", &createSlugMaskShader},
    {"ImageBlit", &createImageBlitShader},
};

class GeodeShaderLinkageTests : public testing::TestWithParam<CreatorCase> {};

INSTANTIATE_TEST_SUITE_P(Creators, GeodeShaderLinkageTests, testing::ValuesIn(kCreators),
                         [](const testing::TestParamInfo<CreatorCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_P(GeodeShaderLinkageTests, WgslDeviceIsRefusedBecauseTheBuildLinksNoWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  EXPECT_THAT(GetParam().create(device),
              gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::InvalidDescriptor,
                                         HasSubstr("sourceText is empty")));
}

TEST_P(GeodeShaderLinkageTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  EXPECT_THAT(GetParam().create(device), gpu::HasResult());

  EXPECT_THAT(device.lastDescriptor(), CarriesANativeProjection());
}

TEST(GeodeSnapshotReadbackLinkageTests, WgslDeviceIsRefusedBecauseTheBuildLinksNoWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  const GeodeSnapshotReadbackPipeline pipeline(device);

  EXPECT_THAT(pipeline.valid(), IsFalse());
  EXPECT_THAT(device.lastDescriptor().sourceText, IsEmpty());
}

TEST(GeodeSnapshotReadbackLinkageTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  const GeodeSnapshotReadbackPipeline pipeline(device);

  EXPECT_THAT(pipeline.valid(), IsTrue());
  EXPECT_THAT(device.lastDescriptor(), CarriesANativeProjection());
}

}  // namespace
}  // namespace donner::geode
