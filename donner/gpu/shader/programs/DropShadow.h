#pragma once
/// @file
/// feDropShadow parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/Offset.h"
namespace donner::gpu::shader::programs {
/// Uniform straight flood color and pixel offset, split per axis by \ref SplitPixelShift.
struct alignas(16) DropShadowParams {
  float color[4];  //!< Straight RGBA flood color; the shader premultiplies.
  int32_t shiftX;  //!< Whole-pixel base of the shadow offset along x.
  int32_t shiftY;  //!< Whole-pixel base of the shadow offset along y.
  float weightX;   //!< Constant blend weight along x.
  float weightY;   //!< Constant blend weight along y.
};
static_assert(sizeof(DropShadowParams) == 32);
/// Returns the WGSL drop-shadow artifact: tinted offset blurred alpha under the source.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DropShadowShader();
/// Returns only the platform-native DropShadow projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DropShadowNativeShader();
}  // namespace donner::gpu::shader::programs
