#pragma once
/// @file
/// Precompiled diffuse lighting projections over the shared lighting parameter layout.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/LightingParams.h"
namespace donner::gpu::shader::programs {
/// Returns the WGSL diffuse-lighting artifact for distant, point and spot lights.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DiffuseLightingShader();
/// Returns only the platform-native DiffuseLighting projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& DiffuseLightingNativeShader();
}  // namespace donner::gpu::shader::programs
