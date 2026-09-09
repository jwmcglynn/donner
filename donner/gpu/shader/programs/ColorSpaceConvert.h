#pragma once
/// @file
/// The sRGB-to-linear filter color space conversion, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ColorSpaceConvertBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the color space conversion program: one `@compute @workgroup_size(8, 8, 1)` entry point
 * named `cs_main` that rewrites every texel from sRGB encoding into linear light, or back, as the
 * direction parameter selects.
 *
 * The specification's `color-interpolation-filters` property makes linear light the default space
 * a filter chain computes in, so a chain converts on the way in and back on the way out. The
 * transfer is the sRGB one: a linear segment below a breakpoint and a power curve above it, with
 * the breakpoint expressed in the space being read. Both halves are exactly the constants the CPU
 * filter path uses, because the two implementations have to agree texel for texel.
 *
 * The curve is a real branch rather than a select, because a select evaluates both arms and `pow`
 * is undefined for a negative base. The linear segment is what covers those inputs.
 *
 * The chain carries premultiplied color while the transfer is defined on straight-alpha values,
 * so the program divides through by alpha, converts the three color channels, and re-associates.
 * Alpha itself is not a color channel and is carried across unchanged.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildColorSpaceConvertModule();

}  // namespace donner::gpu::shader::programs
