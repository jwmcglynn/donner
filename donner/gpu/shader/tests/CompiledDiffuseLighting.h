#pragma once
/// @file
/// All-projection DiffuseLighting controls and reflected-interface mutations.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns every projection of the production source.
/// @return Stable process-lifetime view.
const CompiledShaderView& DiffuseLightingAllProjections();
/// Returns changed bindings, entry names and applicable workgroup shape.
/// @return Stable process-lifetime view.
const CompiledShaderView& DiffuseLightingMutatedAllProjections();
}  // namespace donner::gpu::shader::tests
