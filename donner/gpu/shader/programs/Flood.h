#pragma once
/// @file
/// The flood compute program, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/FloodBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the flood compute program: one `@compute @workgroup_size(8, 8, 1)` entry point named
 * `cs_main` that writes one uniform color to every texel of a write-only storage texture.
 *
 * The program reads no texture, so the destination extent is the only thing that bounds it. The
 * color is written exactly as the uniform carries it: whether it is premultiplied is the caller's
 * decision, and nothing here re-associates it with the alpha channel.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildFloodModule();

}  // namespace donner::gpu::shader::programs
