#pragma once
/// @file
/// SpecularLighting shader artifacts retained for cross-platform acceptance tests.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns process-lifetime views of all specular lighting shader projections.
const CompiledShaderView& SpecularLightingAllProjections();
/// Returns a shader with changed parameter binding and workgroup shape.
const CompiledShaderView& SpecularLightingMutatedAllProjections();
/// Returns a vector sin/cos/pow fixture with exactly representable quantized results.
const CompiledShaderView& LightingMathAllProjections();
}  // namespace donner::gpu::shader::tests
