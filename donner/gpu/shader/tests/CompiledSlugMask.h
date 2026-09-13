#pragma once
/// @file
/// Full Slug mask artifacts for cross-projection validation.

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::tests {
/// Returns the full mask in every projection for tests only.
const CompiledShaderView& SlugMaskAllProjections();
}  // namespace donner::gpu::shader::tests
