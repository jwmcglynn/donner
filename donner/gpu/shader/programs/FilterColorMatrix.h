#pragma once
/// @file
/// The SVG color-matrix filter primitive, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/FilterColorMatrixBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the SVG color-matrix filter program: one `@compute @workgroup_size(8, 8, 1)` entry point
 * named `cs_main` that applies a four-by-five color matrix to every texel.
 *
 * The matrix arrives as five columns: four of per-input-channel multipliers and a constant column
 * added after them. Every matrix form the specification defines - the explicit one, saturation,
 * hue rotation, and luminance-to-alpha - is baked into those five columns by the caller, so this
 * program has one path rather than a branch per form.
 *
 * The specification defines the operation on straight-alpha values while the filter chain carries
 * premultiplied ones, so the program un-premultiplies, applies the matrix, clamps each channel to
 * the representable range, and premultiplies again. A fully transparent source texel has no
 * straight-alpha value to recover, so it is handled directly: the constant column is the whole
 * result there, and it produces transparent black unless that column raises alpha.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildFilterColorMatrixModule();

}  // namespace donner::gpu::shader::programs
