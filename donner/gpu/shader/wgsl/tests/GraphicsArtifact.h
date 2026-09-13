#pragma once
/// @file
/// Frozen graphics shader used by compiler and native validation tests.

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::wgsl::tests {

/// Returns an all-projection graphics test artifact with static storage duration.
const CompiledShaderView& GraphicsShader();

/// Returns an all-projection matrix graphics artifact with static storage duration.
const CompiledShaderView& MatrixShader();

/// Returns an all-projection matrix-operation artifact with static storage duration.
const CompiledShaderView& MatrixOperationsShader();

}  // namespace donner::gpu::shader::wgsl::tests
