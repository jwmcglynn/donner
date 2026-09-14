#pragma once
/// @file
/// feTile parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform source rectangle in pixels; non-positive extents produce transparent output.
struct TileParams {
  int32_t srcX;  //!< Source rectangle left edge.
  int32_t srcY;  //!< Source rectangle top edge.
  int32_t srcW;  //!< Source rectangle width.
  int32_t srcH;  //!< Source rectangle height.
};
static_assert(sizeof(TileParams) == 16);
/// Returns the WGSL tile artifact: wraparound sampling of a signed source rectangle.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& TileShader();
/// Returns only the platform-native Tile projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& TileNativeShader();
}  // namespace donner::gpu::shader::programs
