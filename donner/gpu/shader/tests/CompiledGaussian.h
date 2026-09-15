#pragma once
/// @file
/// The Gaussian shader compiled for every projection, for cross-platform validation tests.

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::tests {

/// Returns process-lifetime views of all Gaussian shader projections.
const CompiledShaderView& GaussianBlurAllProjections();

/// Returns projections with the parameter binding and workgroup shape intentionally changed.
const CompiledShaderView& GaussianBlurMutatedAllProjections();

}  // namespace donner::gpu::shader::tests
