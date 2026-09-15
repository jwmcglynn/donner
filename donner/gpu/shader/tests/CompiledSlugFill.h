#pragma once
/// @file
/// Slug fill test-only projections and interface mutations.
#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::tests {
/// Returns a process-lifetime view of every Slug fill projection.
const CompiledShaderView& SlugFillAllProjections();
/// Returns changed binding slots and ordinary entry-point names.
const CompiledShaderView& SlugFillMutatedAllProjections();
/// Returns a first-vertex flat-interpolation and instance-base fixture.
const CompiledShaderView& SlugFlatInterfaceAllProjections();
}  // namespace donner::gpu::shader::tests
