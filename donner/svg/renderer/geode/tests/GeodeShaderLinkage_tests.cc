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
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/LinkedProjection.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/DiffuseLighting.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/programs/FilterBlend.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/FilterResolve.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SpecularLighting.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
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

/// The projection a native device on this platform consumes, which is the one this build links.
constexpr gpu::ShaderSourceKind kPlatformNativeKind = gpu::shader::kLinkedShaderSourceKind;

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

/// One compute family the filter engine builds a reflected program for, by its linked artifact.
struct FilterProgramCase {
  std::string_view name;                               //!< Test case name and program label.
  const gpu::shader::CompiledShaderView& (*linked)();  //!< The artifact this build links.
};

/// Prints the family name. @param os Output stream. @param program Case to print.
std::ostream& operator<<(std::ostream& os, const FilterProgramCase& program) {
  return os << program.name;
}

/// Every compute family the filter engine builds a program for.
const FilterProgramCase kFilterPrograms[] = {
    {"GaussianBlur", &gpu::shader::programs::GaussianBlurNativeShader},
    {"Offset", &gpu::shader::programs::OffsetNativeShader},
    {"FilterColorMatrix", &gpu::shader::programs::FilterColorMatrixNativeShader},
    {"Flood", &gpu::shader::programs::FloodNativeShader},
    {"Merge", &gpu::shader::programs::MergeNativeShader},
    {"Composite", &gpu::shader::programs::CompositeNativeShader},
    {"FilterBlend", &gpu::shader::programs::FilterBlendNativeShader},
    {"Morphology", &gpu::shader::programs::MorphologyNativeShader},
    {"ComponentTransfer", &gpu::shader::programs::ComponentTransferNativeShader},
    {"ConvolveMatrix", &gpu::shader::programs::ConvolveMatrixNativeShader},
    {"Turbulence", &gpu::shader::programs::TurbulenceNativeShader},
    {"DisplacementMap", &gpu::shader::programs::DisplacementMapNativeShader},
    {"DiffuseLighting", &gpu::shader::programs::DiffuseLightingNativeShader},
    {"SpecularLighting", &gpu::shader::programs::SpecularLightingNativeShader},
    {"DropShadow", &gpu::shader::programs::DropShadowNativeShader},
    {"FilterImage", &gpu::shader::programs::FilterImageNativeShader},
    {"Tile", &gpu::shader::programs::TileNativeShader},
    {"SubregionClip", &gpu::shader::programs::SubregionClipNativeShader},
    {"FilterResolve", &gpu::shader::programs::FilterResolveNativeShader},
    {"ColorSpaceConvert", &gpu::shader::programs::ColorSpaceConvertNativeShader},
};

class GeodeFilterProgramLinkageTests : public testing::TestWithParam<FilterProgramCase> {};

INSTANTIATE_TEST_SUITE_P(Programs, GeodeFilterProgramLinkageTests,
                         testing::ValuesIn(kFilterPrograms),
                         [](const testing::TestParamInfo<FilterProgramCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_P(GeodeFilterProgramLinkageTests, WgslDeviceBuildsNoProgramBecauseTheBuildLinksNoWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  const RuntimeComputeProgram program =
      CreateReflectedProgram(device, GetParam().linked(), GetParam().name);

  EXPECT_THAT(program.shaderModule.isValid(), IsFalse());
  EXPECT_THAT(program.pipeline.isValid(), IsFalse());
}

TEST_P(GeodeFilterProgramLinkageTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  const RuntimeComputeProgram program =
      CreateReflectedProgram(device, GetParam().linked(), GetParam().name);

  EXPECT_THAT(device.lastDescriptor(), CarriesANativeProjection());
  EXPECT_THAT(program.pipeline.isValid(), IsTrue());
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
