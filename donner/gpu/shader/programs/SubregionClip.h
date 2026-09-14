#pragma once
/// @file
/// Subregion clipping precompiled shader projections over the shared resolve parameters.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/FilterResolve.h"
namespace donner::gpu::shader::programs {
/// The clip and resolve programs share one 48-byte host block; the clip ignores the
/// conversion flag.
using SubregionClipParams = FilterResolveParams;
/// Returns the WGSL subregion-clip artifact: float texels outside the user rectangle become
/// transparent.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& SubregionClipShader();
/// Returns only the platform-native SubregionClip projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& SubregionClipNativeShader();
}  // namespace donner::gpu::shader::programs
