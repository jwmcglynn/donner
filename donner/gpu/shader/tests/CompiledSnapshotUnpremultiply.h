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
/// Returns a one-texel compute fixture that shifts i32 and u32 scalars and vectors left and right
/// by runtime amounts, including amounts at or above 32, shifts an i32 scalar left by a literal,
/// shifts an i32 vector through compound assignment and shifts an abstract value by a runtime
/// amount. The input texel (-12, 3, 35, 5) produces (-4, 268435456, -48, 119) under WGSL's shift
/// semantics.
/// @return Stable process-lifetime view.
const CompiledShaderView& ShiftOperatorsAllProjections();
}  // namespace donner::gpu::shader::tests
