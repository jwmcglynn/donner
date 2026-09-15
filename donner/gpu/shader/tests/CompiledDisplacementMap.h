#pragma once
/// @file
/// All-projection DisplacementMap controls and reflected-interface mutations.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns every projection of the production source.
/// @return Stable process-lifetime view.
const CompiledShaderView& DisplacementMapAllProjections();
/// Returns changed bindings, entry names and applicable workgroup shape.
/// @return Stable process-lifetime view.
const CompiledShaderView& DisplacementMapMutatedAllProjections();
}  // namespace donner::gpu::shader::tests
