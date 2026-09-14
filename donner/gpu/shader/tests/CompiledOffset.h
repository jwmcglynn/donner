#pragma once
/// @file
/// Offset shader artifacts retained for cross-platform acceptance tests.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns process-lifetime views of all offset shader projections.
const CompiledShaderView& OffsetAllProjections();
/// Returns a shader with changed parameter binding and workgroup shape.
const CompiledShaderView& OffsetMutatedAllProjections();
/// Returns a vector-floor compute fixture using the shared float texture layout.
const CompiledShaderView& FloorAllProjections();
/// Returns a vector-sign compute fixture using the shared float texture layout.
const CompiledShaderView& SignAllProjections();
}  // namespace donner::gpu::shader::tests
