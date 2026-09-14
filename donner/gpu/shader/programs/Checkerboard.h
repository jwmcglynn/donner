#pragma once
/// @file
/// Transparency checkerboard parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform block consumed by both checkerboard entry points.
struct alignas(16) CheckerboardParams {
  float targetSize[2];      //!< Render-target size in device pixels.
  float devicePixelRatio;   //!< Device pixels per logical pixel.
  float checkerSize;        //!< Checker cell size in logical pixels.
  float darkColor[4];       //!< RGBA for odd cells.
  float lightColor[4];      //!< RGBA for even cells.
  float originOffsetPx[2];  //!< Device-pixel offset of the target's top-left from the anchor.
  float padding[2];         //!< Uniform structure tail padding; must be zero.
};
static_assert(sizeof(CheckerboardParams) == 64);
/// Returns the WGSL checkerboard artifact: a full-screen triangle with device-pixel anchored cells.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& CheckerboardShader();
/// Returns only the platform-native Checkerboard projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& CheckerboardNativeShader();
}  // namespace donner::gpu::shader::programs
