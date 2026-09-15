#pragma once
/// @file
/// FilterResolve shader artifacts retained for cross-platform acceptance tests.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns process-lifetime views of all filter resolve shader projections.
const CompiledShaderView& FilterResolveAllProjections();
/// Returns a shader with changed parameter binding and workgroup shape.
const CompiledShaderView& FilterResolveMutatedAllProjections();
}  // namespace donner::gpu::shader::tests
