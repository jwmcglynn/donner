#pragma once
/// @file
/// The SVG merge filter program expressed in the typed shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/MergeBindings.h"

namespace donner::gpu::shader::programs {

/// Builds a bounded 8-by-8 compute program over premultiplied source and destination textures.
/// Applies source-over compositing for each successive merge input.
ShaderResult<IrModule> BuildMergeModule();

}  // namespace donner::gpu::shader::programs
