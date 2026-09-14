#pragma once
/// @file
/// Shared production compute shader cases for interface checks and explicit allocation
/// measurements.

#include <ostream>

#include "donner/gpu/shader/CompiledShader.h"
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
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SpecularLighting.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"

namespace donner::gpu::shader::tests {

/// One production compute artifact and its build-facing name.
struct Program {
  const char* name;                               //!< Build-facing program identifier.
  const CompiledShaderView& (*compiledShader)();  //!< WGSL projection consumed by the adapter.

  /// Builds the runtime descriptor exactly as the production callers do.
  /// @param kind Device-selected projection.
  ShaderModuleDescriptor buildDescriptor(ShaderSourceKind kind) const {
    return MakeShaderDescriptor(compiledShader(), kind, name);
  }
};

inline const Program kPrograms[] = {
    {"snapshot_unpremultiply", programs::SnapshotUnpremultiplyShader},
    {"flood", programs::FloodShader},
    {"subregion_clip", programs::SubregionClipShader},
    {"offset", programs::OffsetShader},
    {"color_space_convert", programs::ColorSpaceConvertShader},
    {"filter_color_matrix", programs::FilterColorMatrixShader},
    {"gaussian_blur", programs::GaussianBlurShader},
    {"merge", programs::MergeShader},
    {"composite", programs::CompositeShader},
    {"morphology", programs::MorphologyShader},
    {"tile", programs::TileShader},
    {"filter_resolve", programs::FilterResolveShader},
    {"component_transfer", programs::ComponentTransferShader},
    {"displacement_map", programs::DisplacementMapShader},
    {"drop_shadow", programs::DropShadowShader},
    {"convolve_matrix", programs::ConvolveMatrixShader},
    {"filter_image", programs::FilterImageShader},
    {"turbulence", programs::TurbulenceShader},
    {"diffuse_lighting", programs::DiffuseLightingShader},
    {"specular_lighting", programs::SpecularLightingShader},
    {"filter_blend", programs::FilterBlendShader},
};

inline void PrintTo(const Program& program, std::ostream* stream) {
  *stream << program.name;
}

}  // namespace donner::gpu::shader::tests
