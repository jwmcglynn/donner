#pragma once
/// @file
/// SVG matrix-convolution compute program expressed in the shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ConvolveMatrixBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Applies a bounded row-major kernel to premultiplied float texels.
 *
 * The host validates positive orders, a coefficient count equal to their product, targets inside
 * the kernel, a finite nonzero divisor, and the fixed kernel capacity before dispatch. Edge mode
 * 0 duplicates, 1 wraps, and 2 samples transparent black. The kernel is rotated 180 degrees as
 * required by SVG. With preserveAlpha set, RGB is convolved in straight-alpha space and then
 * associated with the unchanged source alpha.
 */
ShaderResult<IrModule> BuildConvolveMatrixModule();

}  // namespace donner::gpu::shader::programs
