#pragma once
/// @file
/// feImage parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform row-major 2x3 image-from-output transform and sampling selection.
struct FilterImageParams {
  float m00;              //!< Output-x coefficient of image x.
  float m01;              //!< Output-y coefficient of image x.
  float m02;              //!< Constant term of image x.
  float m10;              //!< Output-x coefficient of image y.
  float m11;              //!< Output-y coefficient of image y.
  float m12;              //!< Constant term of image y.
  uint32_t samplingMode;  //!< Zero smooth, one crisp edges, two pixelated.
  float pixelatedScaleX;  //!< Device pixels per image texel along x.
  float pixelatedScaleY;  //!< Device pixels per image texel along y.
  uint32_t padding;       //!< Reserved layout padding.
};
static_assert(sizeof(FilterImageParams) == 40);
/// Returns the WGSL image artifact: smooth, crisp or pixelated placement through an
/// image-from-output transform.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FilterImageShader();
/// Returns only the platform-native FilterImage projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FilterImageNativeShader();
}  // namespace donner::gpu::shader::programs
