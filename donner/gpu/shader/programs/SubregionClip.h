#pragma once
/// @file
/// The subregion-clip compute program, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/SubregionClipBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the subregion-clip compute program: one `@compute @workgroup_size(8, 8, 1)` entry point
 * named `cs_main` that copies texels inside a rectangle and writes transparent black outside it.
 *
 * The rectangle is axis-aligned in user space, not in the destination's pixel space, so the test
 * runs on the pixel center mapped back through the uniform's inverse transform. That is what
 * makes the clip correct under a rotated transform, where a pixel-space rectangle would not
 * describe the same region. The rectangle is half-open on both axes: a coordinate equal to the
 * low edge is inside, one equal to the high edge is outside, so abutting subregions neither
 * overlap nor leave a seam.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildSubregionClipModule();

}  // namespace donner::gpu::shader::programs
