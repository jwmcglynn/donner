#pragma once
/// @file
/// UI draw-data parameters and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform block consumed by the UI draw entry points.
struct alignas(16) UiDrawParams {
  /// Column-major clip-space-from-logical-pixel projection applied to every UI vertex.
  float clipFromLogical[16];
};
static_assert(sizeof(UiDrawParams) == 64);
/// Returns the WGSL UI draw artifact: one vertex entry and the straight and premultiplied alpha
/// fragment entries.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& UiDrawShader();
/// Returns only the platform-native UI draw projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& UiDrawNativeShader();
}  // namespace donner::gpu::shader::programs
