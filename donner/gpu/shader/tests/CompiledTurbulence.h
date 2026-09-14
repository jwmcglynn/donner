#pragma once
/// @file
/// Turbulence shader artifacts retained for cross-platform acceptance tests.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns process-lifetime views of all turbulence shader projections.
const CompiledShaderView& TurbulenceAllProjections();
/// Returns a shader with changed parameter binding and workgroup shape.
const CompiledShaderView& TurbulenceMutatedAllProjections();
/// Returns an eight-argument helper fixture with exactly representable results.
const CompiledShaderView& EightArgumentCallAllProjections();
}  // namespace donner::gpu::shader::tests
