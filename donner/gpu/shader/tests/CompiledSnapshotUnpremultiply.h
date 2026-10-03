#pragma once
/// @file
/// All-projection SnapshotUnpremultiply controls, reflected-interface mutations and the shift
/// operator fixture behind its half-alpha term.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns every projection of the production source.
/// @return Stable process-lifetime view.
const CompiledShaderView& SnapshotUnpremultiplyAllProjections();
/// Returns changed bindings, entry names and applicable workgroup shape.
/// @return Stable process-lifetime view.
const CompiledShaderView& SnapshotUnpremultiplyMutatedAllProjections();
/// Returns a one-texel compute fixture that applies both shift operators to i32 and u32 scalars
/// and vectors, including compound assignment and runtime amounts at or above 32. The input
/// texel (-12, 3, 35, 5) produces (-4, 268435456, -48, 15) under WGSL's shift semantics.
/// @return Stable process-lifetime view.
const CompiledShaderView& ShiftOperatorsAllProjections();
}  // namespace donner::gpu::shader::tests
