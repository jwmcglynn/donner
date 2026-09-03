#pragma once
/// @file
/// The SVG offset filter primitive, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/OffsetBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the SVG offset filter program: one `@compute @workgroup_size(8, 8, 1)` entry point named
 * `cs_main` that reads each destination texel from the source shifted by a whole number of
 * pixels.
 *
 * The shift arrives in pixels and is rounded half away from zero, which is what the CPU filter
 * path's `std::round` does and what WGSL's `round` does not: `round` is round-half-to-even and
 * disagrees on every exact half, and a filter chain scaled so a shift lands on one would place
 * the whole primitive a pixel away from the reference. The rounding is the shared recipe rather
 * than a composition written here.
 *
 * Source texels outside the sampled texture produce transparent black. The specification defines
 * no edge behaviour for this primitive, so there is no mode to select.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildOffsetModule();

}  // namespace donner::gpu::shader::programs
