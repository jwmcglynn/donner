#include "donner/gpu/shader/tests/ShippedWgslCatalog.h"

#include <array>

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

namespace donner::gpu::shader::tests {
namespace {

constexpr std::array kShippedWgsl = {
    ShippedWgslCase{"checkerboard_artifact", &programs::CheckerboardShader},
    ShippedWgslCase{"color_space_convert_artifact", &programs::ColorSpaceConvertShader},
    ShippedWgslCase{"component_transfer_artifact", &programs::ComponentTransferShader},
    ShippedWgslCase{"composite_artifact", &programs::CompositeShader},
    ShippedWgslCase{"convolve_matrix_artifact", &programs::ConvolveMatrixShader},
    ShippedWgslCase{"diffuse_lighting_artifact", &programs::DiffuseLightingShader},
    ShippedWgslCase{"displacement_map_artifact", &programs::DisplacementMapShader},
    ShippedWgslCase{"drop_shadow_artifact", &programs::DropShadowShader},
    ShippedWgslCase{"filter_blend_artifact", &programs::FilterBlendShader},
    ShippedWgslCase{"filter_color_matrix_artifact", &programs::FilterColorMatrixShader},
    ShippedWgslCase{"filter_image_artifact", &programs::FilterImageShader},
    ShippedWgslCase{"filter_resolve_artifact", &programs::FilterResolveShader},
    ShippedWgslCase{"flood_artifact", &programs::FloodShader},
    ShippedWgslCase{"gaussian_blur_artifact", &programs::GaussianBlurShader},
    ShippedWgslCase{"image_blit_artifact", &programs::ImageBlitShader},
    ShippedWgslCase{"merge_artifact", &programs::MergeShader},
    ShippedWgslCase{"morphology_artifact", &programs::MorphologyShader},
    ShippedWgslCase{"offset_artifact", &programs::OffsetShader},
    ShippedWgslCase{"slug_fill_artifact", &programs::SlugFillShader},
    ShippedWgslCase{"slug_gradient_artifact", &programs::SlugGradientShader},
    ShippedWgslCase{"slug_mask_artifact", &programs::SlugMaskShader},
    ShippedWgslCase{"snapshot_unpremultiply_artifact", &programs::SnapshotUnpremultiplyShader},
    ShippedWgslCase{"specular_lighting_artifact", &programs::SpecularLightingShader},
    ShippedWgslCase{"subregion_clip_artifact", &programs::SubregionClipShader},
    ShippedWgslCase{"tile_artifact", &programs::TileShader},
    ShippedWgslCase{"turbulence_artifact", &programs::TurbulenceShader},
    ShippedWgslCase{"ui_draw_artifact", &programs::UiDrawShader},
};

}  // namespace

std::span<const ShippedWgslCase> ShippedWgslCatalog() {
  return kShippedWgsl;
}

}  // namespace donner::gpu::shader::tests
