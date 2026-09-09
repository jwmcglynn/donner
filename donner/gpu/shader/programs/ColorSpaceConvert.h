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
 * transfer uses 4096 samples per direction, computed in double precision and rounded once to
 * float, matching the software filter path. A shared read-only table avoids backend-dependent
 * power-function results. Comparison-based clamping maps nonpositive and NaN channels to zero
 * before bounded nearest-sample indexing.
 *
 * The chain carries premultiplied color while the transfer is defined on straight-alpha values,
 * so the program multiplies by reciprocal alpha, converts the three color channels, and
 * re-associates. Alpha itself is not a color channel and is carried across unchanged.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildColorSpaceConvertModule();

}  // namespace donner::gpu::shader::programs
