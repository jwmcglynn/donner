#pragma once
/// @file
/// feDropShadow parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform straight flood color and integer-rounded pixel offset.
struct alignas(16) DropShadowParams {
  float color[4];  //!< Straight RGBA flood color; the shader premultiplies.
  float dx;        //!< Horizontal shadow offset in pixels.
  float dy;        //!< Vertical shadow offset in pixels.
  uint32_t pad0;   //!< Reserved layout padding.
  uint32_t pad1;   //!< Reserved layout padding.
};
static_assert(sizeof(DropShadowParams) == 32);
/// Returns the WGSL drop-shadow artifact: tinted offset blurred alpha under the source.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DropShadowShader();
/// Returns only the platform-native DropShadow projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DropShadowNativeShader();
}  // namespace donner::gpu::shader::programs
