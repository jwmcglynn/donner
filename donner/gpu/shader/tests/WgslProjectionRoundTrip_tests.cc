/// @file
/// Checks that every shipped WGSL projection parses back to the native bytes of its family's
/// native artifact, so the text a browser compiles describes the shader the native backends run.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/Checkerboard.h"
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
#include "donner/gpu/shader/programs/UiDraw.h"
#include "donner/gpu/shader/tests/ShippedWgslCatalog.h"
#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader {
namespace {

/// A shipped family's native artifact, keyed by the target of its WGSL artifact.
struct NativeArtifact {
  std::string_view artifactTarget;        //!< WGSL artifact target the catalog lists.
  const CompiledShaderView& (*shader)();  //!< Returns the family's native artifact.
};

constexpr std::array kNativeArtifacts = {
    NativeArtifact{"checkerboard_artifact", &programs::CheckerboardNativeShader},
    NativeArtifact{"color_space_convert_artifact", &programs::ColorSpaceConvertNativeShader},
    NativeArtifact{"component_transfer_artifact", &programs::ComponentTransferNativeShader},
    NativeArtifact{"composite_artifact", &programs::CompositeNativeShader},
    NativeArtifact{"convolve_matrix_artifact", &programs::ConvolveMatrixNativeShader},
    NativeArtifact{"diffuse_lighting_artifact", &programs::DiffuseLightingNativeShader},
    NativeArtifact{"displacement_map_artifact", &programs::DisplacementMapNativeShader},
    NativeArtifact{"drop_shadow_artifact", &programs::DropShadowNativeShader},
    NativeArtifact{"filter_blend_artifact", &programs::FilterBlendNativeShader},
    NativeArtifact{"filter_color_matrix_artifact", &programs::FilterColorMatrixNativeShader},
    NativeArtifact{"filter_image_artifact", &programs::FilterImageNativeShader},
    NativeArtifact{"filter_resolve_artifact", &programs::FilterResolveNativeShader},
    NativeArtifact{"flood_artifact", &programs::FloodNativeShader},
    NativeArtifact{"gaussian_blur_artifact", &programs::GaussianBlurNativeShader},
    NativeArtifact{"image_blit_artifact", &programs::ImageBlitNativeShader},
    NativeArtifact{"merge_artifact", &programs::MergeNativeShader},
    NativeArtifact{"morphology_artifact", &programs::MorphologyNativeShader},
    NativeArtifact{"offset_artifact", &programs::OffsetNativeShader},
    NativeArtifact{"slug_fill_artifact", &programs::SlugFillNativeShader},
    NativeArtifact{"slug_gradient_artifact", &programs::SlugGradientNativeShader},
    NativeArtifact{"slug_mask_artifact", &programs::SlugMaskNativeShader},
    NativeArtifact{"snapshot_unpremultiply_artifact", &programs::SnapshotUnpremultiplyNativeShader},
    NativeArtifact{"specular_lighting_artifact", &programs::SpecularLightingNativeShader},
    NativeArtifact{"subregion_clip_artifact", &programs::SubregionClipNativeShader},
    NativeArtifact{"tile_artifact", &programs::TileNativeShader},
    NativeArtifact{"turbulence_artifact", &programs::TurbulenceNativeShader},
    NativeArtifact{"ui_draw_artifact", &programs::UiDrawNativeShader},
};

/// Returns the native artifact of the family whose WGSL artifact is p artifactTarget, or null.
const CompiledShaderView* NativeArtifactFor(std::string_view artifactTarget) {
  const auto match = std::find_if(
      kNativeArtifacts.begin(), kNativeArtifacts.end(),
      [&](const NativeArtifact& entry) { return entry.artifactTarget == artifactTarget; });
  return match == kNativeArtifacts.end() ? nullptr : &match->shader();
}

class ShippedWgslRoundTripTest : public testing::TestWithParam<tests::ShippedWgslCase> {};

TEST_P(ShippedWgslRoundTripTest, ProjectionParsesBackToTheNativeBytes) {
  const CompiledShaderView* native = NativeArtifactFor(GetParam().artifactTarget);
  ASSERT_NE(native, nullptr) << "No native artifact listed for " << GetParam().artifactTarget;
  const CompiledShaderView& shipped = GetParam().shader();
  ASSERT_FALSE(shipped.wgsl.empty());

  const auto parsed = std::make_unique<wgsl::ParseResult>(wgsl::Parse(shipped.wgsl));
  ASSERT_TRUE(parsed->hasResult())
      << "Rejected shipped WGSL for " << GetParam().artifactTarget << " at bytes "
      << parsed->diagnostic.span.begin << "-" << parsed->diagnostic.span.end << " (error "
      << static_cast<int>(parsed->diagnostic.code) << ")";

  // The native artifact carries this platform's projection, MSL on macOS and SPIR-V on Linux, so
  // the two platforms together compare both.
  ASSERT_FALSE(native->msl.empty() && native->spirv.empty());
  if (!native->msl.empty()) {
    std::vector<char> msl(wgsl::kMaxTextEmitBytes);
    wgsl::TextSink mslSink{msl.data(), static_cast<uint32_t>(msl.size())};
    ASSERT_TRUE(wgsl::EmitMsl(parsed->module, mslSink).ok());
    EXPECT_EQ(mslSink.view(), native->msl)
        << "MSL of the reparsed projection differs for " << GetParam().artifactTarget;
  }
  if (!native->spirv.empty()) {
    std::vector<uint32_t> spirv(wgsl::kMaxSpirvEmitWords);
    wgsl::SpirvSink spirvSink{spirv.data(), static_cast<uint32_t>(spirv.size())};
    ASSERT_EQ(wgsl::EmitSpirv(parsed->module, spirvSink).error, wgsl::SpirvEmitError::None);
    spirv.resize(spirvSink.size);
    EXPECT_TRUE(std::equal(spirv.begin(), spirv.end(), native->spirv.begin(), native->spirv.end()))
        << "SPIR-V of the reparsed projection differs for " << GetParam().artifactTarget;
  }
}

INSTANTIATE_TEST_SUITE_P(Production, ShippedWgslRoundTripTest,
                         testing::ValuesIn(tests::ShippedWgslCatalog()),
                         [](const testing::TestParamInfo<tests::ShippedWgslCase>& info) {
                           return std::string(info.param.artifactTarget);
                         });

}  // namespace
}  // namespace donner::gpu::shader
