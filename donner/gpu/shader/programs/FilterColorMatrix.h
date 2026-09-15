#pragma once
/// @file
/// feColorMatrix parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform 4x5 matrix stored as five columns; the last column is the constant offset.
struct alignas(16) FilterColorMatrixParams {
  float col0[4];  //!< Multipliers applied to the straight red input.
  float col1[4];  //!< Multipliers applied to the straight green input.
  float col2[4];  //!< Multipliers applied to the straight blue input.
  float col3[4];  //!< Multipliers applied to the alpha input.
  float col4[4];  //!< Constant offsets.
};
static_assert(sizeof(FilterColorMatrixParams) == 80);
/// Returns the WGSL color-matrix artifact: a 4x5 straight-alpha matrix with clamping.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FilterColorMatrixShader();
/// Returns only the platform-native FilterColorMatrix projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FilterColorMatrixNativeShader();
}  // namespace donner::gpu::shader::programs
