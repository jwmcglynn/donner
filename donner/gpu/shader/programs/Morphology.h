#pragma once
/// @file
/// feMorphology parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform radii and operator for one erode or dilate pass.
struct MorphologyParams {
  int32_t radiusX;  //!< Horizontal radius in pixels; zero disables the axis.
  int32_t radiusY;  //!< Vertical radius in pixels; zero disables the axis.
  uint32_t op;      //!< Zero erodes, one dilates.
  uint32_t pad;     //!< Reserved layout padding.
};
static_assert(sizeof(MorphologyParams) == 16);
/// Returns the WGSL morphology artifact: separable erode or dilate over a signed radius.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& MorphologyShader();
/// Returns only the platform-native Morphology projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& MorphologyNativeShader();
}  // namespace donner::gpu::shader::programs
