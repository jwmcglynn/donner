#pragma once
/// @file
/// feDisplacementMap parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform displacement scale and channel selectors.
struct DisplacementMapParams {
  float scale;        //!< Displacement scale in pixels.
  uint32_t xChannel;  //!< Map channel driving x: zero red, one green, two blue, three alpha.
  uint32_t yChannel;  //!< Map channel driving y, with the same encoding.
  uint32_t padding;   //!< Reserved layout padding.
};
static_assert(sizeof(DisplacementMapParams) == 16);
/// Returns the WGSL displacement artifact: bilinear source lookups displaced by two map channels.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DisplacementMapShader();
/// Returns only the platform-native DisplacementMap projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DisplacementMapNativeShader();
}  // namespace donner::gpu::shader::programs
