#pragma once
/// @file
/// Shared production shader cases for interface checks and explicit allocation measurements.

#include <ostream>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/generated/ColorSpaceConvertShader.h"
#include "donner/gpu/shader/generated/ComponentTransferShader.h"
#include "donner/gpu/shader/generated/CompositeShader.h"
#include "donner/gpu/shader/generated/DiffuseLightingShader.h"
#include "donner/gpu/shader/generated/DisplacementMapShader.h"
#include "donner/gpu/shader/generated/DropShadowShader.h"
#include "donner/gpu/shader/generated/FilterColorMatrixShader.h"
#include "donner/gpu/shader/generated/FilterImageShader.h"
#include "donner/gpu/shader/generated/FilterResolveShader.h"
#include "donner/gpu/shader/generated/FloodShader.h"
#include "donner/gpu/shader/generated/MergeShader.h"
#include "donner/gpu/shader/generated/MorphologyShader.h"
#include "donner/gpu/shader/generated/SnapshotUnpremultiplyShader.h"
#include "donner/gpu/shader/generated/SpecularLightingShader.h"
#include "donner/gpu/shader/generated/SubregionClipShader.h"
#include "donner/gpu/shader/generated/TileShader.h"
#include "donner/gpu/shader/generated/TurbulenceShader.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/Lighting.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"

namespace donner::gpu::shader::tests {

struct Program {
  const char* name;
  ShaderResult<IrModule> (*buildModule)();
  ShaderModuleDescriptor (*buildDescriptor)(ShaderSourceKind);
  bool nativeSources;
  const CompiledShaderView& (*compiledShader)() = nullptr;
};

inline const Program kPrograms[] = {
    {"snapshot_unpremultiply", programs::BuildSnapshotUnpremultiplyModule,
     generated::snapshot_unpremultiply::BuildDescriptor, false},
    {"flood", programs::BuildFloodModule, generated::flood::BuildDescriptor, false},
    {"subregion_clip", programs::BuildSubregionClipModule,
     generated::subregion_clip::BuildDescriptor, false},
    {"offset", nullptr,
     [](ShaderSourceKind kind) {
       return MakeShaderDescriptor(programs::OffsetShader(), kind, "Offset");
     },
     false, programs::OffsetShader},
    {"color_space_convert", programs::BuildColorSpaceConvertModule,
     generated::color_space_convert::BuildDescriptor, false},
    {"filter_color_matrix", programs::BuildFilterColorMatrixModule,
     generated::filter_color_matrix::BuildDescriptor, false},
    {"gaussian_blur", nullptr,
     [](ShaderSourceKind kind) {
       return MakeShaderDescriptor(programs::GaussianBlurShader(), kind, "GaussianBlur");
     },
     true, programs::GaussianBlurShader},
    {"merge", programs::BuildMergeModule, generated::merge::BuildDescriptor, false},
    {"composite", programs::BuildCompositeModule, generated::composite::BuildDescriptor, false},
    {"morphology", programs::BuildMorphologyModule, generated::morphology::BuildDescriptor, false},
    {"tile", programs::BuildTileModule, generated::tile::BuildDescriptor, false},
    {"filter_resolve", programs::BuildFilterResolveModule,
     generated::filter_resolve::BuildDescriptor, false},
    {"component_transfer", programs::BuildComponentTransferModule,
     generated::component_transfer::BuildDescriptor, true},
    {"displacement_map", programs::BuildDisplacementMapModule,
     generated::displacement_map::BuildDescriptor, true},
    {"drop_shadow", programs::BuildDropShadowModule, generated::drop_shadow::BuildDescriptor, true},
    {"convolve_matrix", nullptr,
     [](ShaderSourceKind kind) {
       return MakeShaderDescriptor(programs::ConvolveMatrixShader(), kind, "ConvolveMatrix");
     },
     true, programs::ConvolveMatrixShader},
    {"filter_image", programs::BuildFilterImageModule, generated::filter_image::BuildDescriptor,
     true},
    {"turbulence", programs::BuildTurbulenceModule, generated::turbulence::BuildDescriptor, true},
    {"diffuse_lighting", programs::BuildDiffuseLightingModule,
     generated::diffuse_lighting::BuildDescriptor, true},
    {"specular_lighting", programs::BuildSpecularLightingModule,
     generated::specular_lighting::BuildDescriptor, true},
};

inline void PrintTo(const Program& program, std::ostream* stream) {
  *stream << program.name;
}

}  // namespace donner::gpu::shader::tests
