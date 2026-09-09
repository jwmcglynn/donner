#pragma once
/// @file
/// The SVG composite filter program expressed in the typed shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/CompositeBindings.h"

namespace donner::gpu::shader::programs {

/// Builds a bounded 8-by-8 compute program over premultiplied source and destination textures.
/// Applies the selected Porter-Duff or arithmetic operator, clamping each output channel.
ShaderResult<IrModule> BuildCompositeModule();

}  // namespace donner::gpu::shader::programs
