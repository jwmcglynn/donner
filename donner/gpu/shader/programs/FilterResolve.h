#pragma once
/// @file
/// Filter resolve parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Combined forward and inverse transfer-table length.
inline constexpr uint32_t kFilterResolveTransferCount = 8192;
/// Shared 48-byte transform, half-open clipping region and conversion flags.
struct FilterResolveParams {
  float invA;     //!< Pixel-x coefficient of user-space x.
  float invB;     //!< Pixel-x coefficient of user-space y.
  float invC;     //!< Pixel-y coefficient of user-space x.
  float invD;     //!< Pixel-y coefficient of user-space y.
  float invE;     //!< User-space x translation.
  float invF;     //!< User-space y translation.
  float userX0;   //!< Inclusive low x clip edge.
  float userY0;   //!< Inclusive low y clip edge.
  float userX1;   //!< Exclusive high x clip edge.
  float userY1;   //!< Exclusive high y clip edge.
  uint32_t pad0;  //!< Nonzero converts straight linear RGB through the sRGB lookup table.
  uint32_t pad1;  //!< Reserved uniform padding.
};
static_assert(sizeof(FilterResolveParams) == 48);
/// Returns WGSL and reflection for float-to-RGBA8 resolve with optional linear-to-sRGB transfer.
/// The shader clips in user space, preserves premultiplication and rounds clamped channels half up.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& FilterResolveShader();
/// Returns only the platform-native MSL or SPIR-V projection and reflection.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& FilterResolveNativeShader();
}  // namespace donner::gpu::shader::programs
