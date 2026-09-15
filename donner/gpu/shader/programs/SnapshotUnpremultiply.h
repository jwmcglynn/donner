#pragma once
/// @file
/// Snapshot unpremultiply precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Returns the WGSL snapshot artifact: premultiplied float texels become rounded straight RGBA8.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& SnapshotUnpremultiplyShader();
/// Returns only the platform-native SnapshotUnpremultiply projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& SnapshotUnpremultiplyNativeShader();
}  // namespace donner::gpu::shader::programs
