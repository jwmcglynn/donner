#pragma once
/// @file
/// feFlood parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform flood color. The bytes are stored exactly as supplied; the host premultiplies.
struct alignas(16) FloodParams {
  float color[4];  //!< Premultiplied RGBA written to every destination texel.
};
static_assert(sizeof(FloodParams) == 16);
/// Returns the WGSL flood artifact, which writes one premultiplied color to every texel.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FloodShader();
/// Returns only the platform-native Flood projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FloodNativeShader();
}  // namespace donner::gpu::shader::programs
