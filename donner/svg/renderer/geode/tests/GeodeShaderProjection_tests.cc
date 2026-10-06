/// @file
/// Projection selection for every production Geode shader module: the Slug and image-blit
/// module creators, the filter engine's reflected compute programs, and the snapshot readback
/// pipeline.
///
/// Production code names only the projection this build links. This test links the test-only
/// WGSL alternates, so a device consuming WGSL still receives each family's authored WGSL;
/// `GeodeShaderLinkage_tests.cc` covers the same creators without them.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <ostream>
#include <span>
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
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
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
using testing::ElementsAreArray;
using testing::Eq;
using testing::Field;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::Not;
using tests::ProjectionCapturingDevice;

/// The projection a native device on this platform consumes. The native artifacts this build
/// links carry exactly this one.
constexpr gpu::ShaderSourceKind kPlatformNativeKind = gpu::shader::kLinkedShaderSourceKind;

/// The native projection no artifact this build links carries, so a device reporting it must be
/// refused rather than handed an empty source.
constexpr gpu::ShaderSourceKind kUnlinkedNativeKind =
#if defined(__APPLE__)
    gpu::ShaderSourceKind::Spirv;
#else
    gpu::ShaderSourceKind::Msl;
#endif

/**
 * Matches a shader module descriptor carrying \p artifact's projection for this platform's
 * native source kind, and reports which half disagreed when it does not.
 *
 * The projection must also be non-empty, so an artifact that links nothing for this platform
 * cannot satisfy the match by comparing empty against empty.
 */
MATCHER_P(CarriesTheNativeProjectionOf, artifact,
          "carries the platform-native projection of the artifact") {
  if constexpr (kPlatformNativeKind == gpu::ShaderSourceKind::Msl) {
    return ExplainMatchResult(
        AllOf(Field("sourceKind", &gpu::ShaderModuleDescriptor::sourceKind, kPlatformNativeKind),
              Field("sourceText", &gpu::ShaderModuleDescriptor::sourceText,
                    AllOf(Not(IsEmpty()), Eq(artifact.msl))),
              Field("spirvWords", &gpu::ShaderModuleDescriptor::spirvWords, IsEmpty())),
        arg, result_listener);
  } else {
    return ExplainMatchResult(
        AllOf(Field("sourceKind", &gpu::ShaderModuleDescriptor::sourceKind, kPlatformNativeKind),
              Field("sourceText", &gpu::ShaderModuleDescriptor::sourceText, IsEmpty()),
              Field("spirvWords", &gpu::ShaderModuleDescriptor::spirvWords,
                    AllOf(Not(IsEmpty()), ElementsAreArray(artifact.spirv)))),
        arg, result_listener);
  }
}

/**
 * Matches a shader module descriptor carrying \p artifact's authored WGSL.
 */
MATCHER_P(CarriesTheAuthoredWgslOf, artifact, "carries the authored WGSL of the artifact") {
  return ExplainMatchResult(
      AllOf(Field("sourceKind", &gpu::ShaderModuleDescriptor::sourceKind,
                  gpu::ShaderSourceKind::Wgsl),
            Field("sourceText", &gpu::ShaderModuleDescriptor::sourceText,
                  AllOf(Not(IsEmpty()), Eq(artifact.wgsl))),
            Field("spirvWords", &gpu::ShaderModuleDescriptor::spirvWords, IsEmpty())),
      arg, result_listener);
}

/// One production family: the creator under test, the artifact the build links for it, and the
/// authored WGSL the test-only alternates supply.
struct FamilyCase {
  std::string_view name;                                   //!< Test case name.
  gpu::Result<gpu::ShaderModule> (*create)(gpu::Device&);  //!< Module creator under test.
  const gpu::shader::CompiledShaderView& (*wgsl)();        //!< Authored WGSL artifact.
  const gpu::shader::CompiledShaderView& (*native)();      //!< Platform-native artifact.
};

/// Prints the family name. @param os Output stream. @param family Case to print.
std::ostream& operator<<(std::ostream& os, const FamilyCase& family) {
  return os << family.name;
}

/// Every family whose module the Geode renderer creates through `GeodeShaders.h`.
const FamilyCase kFamilies[] = {
    {"SlugFill", &createSlugFillShader, &gpu::shader::programs::SlugFillShader,
     &gpu::shader::programs::SlugFillNativeShader},
    {"SlugGradient", &createSlugGradientShader, &gpu::shader::programs::SlugGradientShader,
     &gpu::shader::programs::SlugGradientNativeShader},
    {"SlugMask", &createSlugMaskShader, &gpu::shader::programs::SlugMaskShader,
     &gpu::shader::programs::SlugMaskNativeShader},
    {"ImageBlit", &createImageBlitShader, &gpu::shader::programs::ImageBlitShader,
     &gpu::shader::programs::ImageBlitNativeShader},
};

class GeodeShaderProjectionTests : public testing::TestWithParam<FamilyCase> {};

INSTANTIATE_TEST_SUITE_P(Families, GeodeShaderProjectionTests, testing::ValuesIn(kFamilies),
                         [](const testing::TestParamInfo<FamilyCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_P(GeodeShaderProjectionTests, WgslDeviceReceivesTheAuthoredWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  EXPECT_THAT(GetParam().create(device), gpu::HasResult());

  EXPECT_THAT(device.lastDescriptor(), CarriesTheAuthoredWgslOf(GetParam().wgsl()));
}

TEST_P(GeodeShaderProjectionTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  EXPECT_THAT(GetParam().create(device), gpu::HasResult());

  EXPECT_THAT(device.lastDescriptor(), CarriesTheNativeProjectionOf(GetParam().native()));
}

TEST_P(GeodeShaderProjectionTests, SourceKindTheLinkedArtifactLacksIsRefused) {
  ProjectionCapturingDevice device(kUnlinkedNativeKind);
  const std::string expectedMessage = kUnlinkedNativeKind == gpu::ShaderSourceKind::Spirv
                                          ? "spirvWords is empty"
                                          : "sourceText is empty";

  EXPECT_THAT(
      GetParam().create(device),
      gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::InvalidDescriptor, HasSubstr(expectedMessage)));
}

/// One compute family the filter engine builds a reflected program for.
struct FilterProgramCase {
  std::string_view name;                               //!< Test case name and program label.
  const gpu::shader::CompiledShaderView& (*wgsl)();    //!< Authored WGSL artifact.
  const gpu::shader::CompiledShaderView& (*native)();  //!< Platform-native artifact.
};

/// Prints the family name. @param os Output stream. @param program Case to print.
std::ostream& operator<<(std::ostream& os, const FilterProgramCase& program) {
  return os << program.name;
}

/// Every compute family the filter engine builds a program for, in constructor order.
const FilterProgramCase kFilterPrograms[] = {
    {"GaussianBlur", &gpu::shader::programs::GaussianBlurShader,
     &gpu::shader::programs::GaussianBlurNativeShader},
    {"Offset", &gpu::shader::programs::OffsetShader, &gpu::shader::programs::OffsetNativeShader},
    {"FilterColorMatrix", &gpu::shader::programs::FilterColorMatrixShader,
     &gpu::shader::programs::FilterColorMatrixNativeShader},
    {"Flood", &gpu::shader::programs::FloodShader, &gpu::shader::programs::FloodNativeShader},
    {"Merge", &gpu::shader::programs::MergeShader, &gpu::shader::programs::MergeNativeShader},
    {"Composite", &gpu::shader::programs::CompositeShader,
     &gpu::shader::programs::CompositeNativeShader},
    {"FilterBlend", &gpu::shader::programs::FilterBlendShader,
     &gpu::shader::programs::FilterBlendNativeShader},
    {"Morphology", &gpu::shader::programs::MorphologyShader,
     &gpu::shader::programs::MorphologyNativeShader},
    {"ComponentTransfer", &gpu::shader::programs::ComponentTransferShader,
     &gpu::shader::programs::ComponentTransferNativeShader},
    {"ConvolveMatrix", &gpu::shader::programs::ConvolveMatrixShader,
     &gpu::shader::programs::ConvolveMatrixNativeShader},
    {"Turbulence", &gpu::shader::programs::TurbulenceShader,
     &gpu::shader::programs::TurbulenceNativeShader},
    {"DisplacementMap", &gpu::shader::programs::DisplacementMapShader,
     &gpu::shader::programs::DisplacementMapNativeShader},
    {"DiffuseLighting", &gpu::shader::programs::DiffuseLightingShader,
     &gpu::shader::programs::DiffuseLightingNativeShader},
    {"SpecularLighting", &gpu::shader::programs::SpecularLightingShader,
     &gpu::shader::programs::SpecularLightingNativeShader},
    {"DropShadow", &gpu::shader::programs::DropShadowShader,
     &gpu::shader::programs::DropShadowNativeShader},
    {"FilterImage", &gpu::shader::programs::FilterImageShader,
     &gpu::shader::programs::FilterImageNativeShader},
    {"Tile", &gpu::shader::programs::TileShader, &gpu::shader::programs::TileNativeShader},
    {"SubregionClip", &gpu::shader::programs::SubregionClipShader,
     &gpu::shader::programs::SubregionClipNativeShader},
    {"FilterResolve", &gpu::shader::programs::FilterResolveShader,
     &gpu::shader::programs::FilterResolveNativeShader},
    {"ColorSpaceConvert", &gpu::shader::programs::ColorSpaceConvertShader,
     &gpu::shader::programs::ColorSpaceConvertNativeShader},
};

class GeodeFilterProgramProjectionTests : public testing::TestWithParam<FilterProgramCase> {};

INSTANTIATE_TEST_SUITE_P(Programs, GeodeFilterProgramProjectionTests,
                         testing::ValuesIn(kFilterPrograms),
                         [](const testing::TestParamInfo<FilterProgramCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_P(GeodeFilterProgramProjectionTests, WgslDeviceReceivesTheAuthoredWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  const RuntimeComputeProgram program =
      CreateReflectedProgram(device, GetParam().native(), GetParam().name);

  EXPECT_THAT(device.lastDescriptor(), CarriesTheAuthoredWgslOf(GetParam().wgsl()));
  EXPECT_THAT(program.pipeline.isValid(), IsTrue());
}

TEST_P(GeodeFilterProgramProjectionTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  const RuntimeComputeProgram program =
      CreateReflectedProgram(device, GetParam().native(), GetParam().name);

  EXPECT_THAT(device.lastDescriptor(), CarriesTheNativeProjectionOf(GetParam().native()));
  EXPECT_THAT(program.pipeline.isValid(), IsTrue());
}

TEST_P(GeodeFilterProgramProjectionTests, SourceKindTheLinkedArtifactLacksBuildsNoProgram) {
  ProjectionCapturingDevice device(kUnlinkedNativeKind);

  const RuntimeComputeProgram program =
      CreateReflectedProgram(device, GetParam().native(), GetParam().name);

  EXPECT_THAT(program.shaderModule.isValid(), IsFalse());
  EXPECT_THAT(program.pipeline.isValid(), IsFalse());
}

TEST(GeodeSnapshotReadbackProjectionTests, WgslDeviceReceivesTheAuthoredWgsl) {
  ProjectionCapturingDevice device(gpu::ShaderSourceKind::Wgsl);

  const GeodeSnapshotReadbackPipeline pipeline(device);

  EXPECT_THAT(pipeline.valid(), IsTrue());
  EXPECT_THAT(device.lastDescriptor(),
              CarriesTheAuthoredWgslOf(gpu::shader::programs::SnapshotUnpremultiplyShader()));
}

TEST(GeodeSnapshotReadbackProjectionTests, NativeDeviceReceivesThePlatformProjection) {
  ProjectionCapturingDevice device(kPlatformNativeKind);

  const GeodeSnapshotReadbackPipeline pipeline(device);

  EXPECT_THAT(pipeline.valid(), IsTrue());
  EXPECT_THAT(
      device.lastDescriptor(),
      CarriesTheNativeProjectionOf(gpu::shader::programs::SnapshotUnpremultiplyNativeShader()));
}

TEST(GeodeSnapshotReadbackProjectionTests, SourceKindTheLinkedArtifactLacksIsRefused) {
  ProjectionCapturingDevice device(kUnlinkedNativeKind);

  const GeodeSnapshotReadbackPipeline pipeline(device);

  EXPECT_THAT(pipeline.valid(), IsFalse());
  EXPECT_THAT(device.lastDescriptor().sourceText, IsEmpty());
}

}  // namespace
}  // namespace donner::geode
