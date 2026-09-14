#pragma once
/// @file
/// feMerge precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Returns the WGSL merge artifact: one premultiplied source-over pass of two inputs.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& MergeShader();
/// Returns only the platform-native Merge projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& MergeNativeShader();
}  // namespace donner::gpu::shader::programs
