#pragma once
/// @file
/// All-projection FilterBlend controls and reflected-interface mutations.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns every projection of the production source.
/// @return Stable process-lifetime view.
const CompiledShaderView& FilterBlendAllProjections();
/// Returns changed bindings, entry names and applicable workgroup shape.
/// @return Stable process-lifetime view.
const CompiledShaderView& FilterBlendMutatedAllProjections();
}  // namespace donner::gpu::shader::tests
