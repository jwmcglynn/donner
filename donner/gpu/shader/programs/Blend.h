#pragma once
/// @file
/// Typed W3C blend modes over premultiplied float textures.
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/BlendBindings.h"
namespace donner::gpu::shader::programs {
/// Builds all sixteen standard blend modes. Unknown modes use normal source-over.
/// Inputs and output have matching extents, and their channels are premultiplied RGBA.
ShaderResult<IrModule> BuildBlendModule();
}  // namespace donner::gpu::shader::programs
