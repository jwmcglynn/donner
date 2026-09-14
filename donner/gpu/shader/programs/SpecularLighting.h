#pragma once
/// @file
/// Precompiled specular lighting projections with a shared storage parameter layout.
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/LightingBindings.h"
namespace donner::gpu::shader::programs {
/// Returns WGSL and reflection for distant, point and spot Phong lighting.
/// The shader reconstructs normals from bounded alpha samples and uses maximum RGB as alpha.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& SpecularLightingShader();
/// Returns only the platform-native MSL or SPIR-V projection and reflection.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& SpecularLightingNativeShader();
}  // namespace donner::gpu::shader::programs
