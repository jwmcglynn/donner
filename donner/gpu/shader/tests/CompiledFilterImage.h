#pragma once
/// @file
/// All-projection FilterImage controls and reflected-interface mutations.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns every projection of the production source.
/// @return Stable process-lifetime view.
const CompiledShaderView& FilterImageAllProjections();
/// Returns changed bindings, entry names and applicable workgroup shape.
/// @return Stable process-lifetime view.
const CompiledShaderView& FilterImageMutatedAllProjections();
}  // namespace donner::gpu::shader::tests
