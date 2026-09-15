#pragma once
/// @file
/// Image-blit artifacts retained for cross-platform acceptance.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns process-lifetime views of all image-blit projections.
const CompiledShaderView& ImageBlitAllProjections();
/// Returns changed resource bindings and entry-point names.
const CompiledShaderView& ImageBlitMutatedAllProjections();
/// Returns explicit-level sampling under varying fragment control.
const CompiledShaderView& ImageBlitExplicitLevelAllProjections();
/// Returns the ArraySwitch native compiler acceptance fixture.
const CompiledShaderView& ArraySwitchAllProjections();
/// Returns the LoopSwitch native compiler acceptance fixture.
const CompiledShaderView& LoopSwitchAllProjections();
/// Returns the VectorMix native compiler acceptance fixture.
const CompiledShaderView& VectorMixAllProjections();
/// Returns zero, scalar-member and vector-member structure construction.
const CompiledShaderView& StructConstructionAllProjections();
/// Returns function-address-space pointer parameters over a bool-bearing value structure.
const CompiledShaderView& PointerStructAllProjections();
/// Returns `loop`/`while` control flow with integer and float compound assignments.
const CompiledShaderView& LoopWhileAllProjections();
}  // namespace donner::gpu::shader::tests
