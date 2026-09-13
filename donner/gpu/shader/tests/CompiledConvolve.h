#pragma once
/// @file
/// The Convolve Matrix shader compiled for every projection in validation tests.

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::tests {

/// Returns process-lifetime views of all Convolve Matrix shader projections.
const CompiledShaderView& ConvolveMatrixAllProjections();

/// Returns projections with parameter binding and workgroup dimensions changed for reflection
/// tests.
const CompiledShaderView& ConvolveMatrixMutatedAllProjections();

/// Returns projections whose runtime coefficient index is biased above the array range.
const CompiledShaderView& ConvolveMatrixHighIndexAllProjections();

/// Returns projections whose runtime coefficient index is biased below the array range.
const CompiledShaderView& ConvolveMatrixLowIndexAllProjections();

}  // namespace donner::gpu::shader::tests
