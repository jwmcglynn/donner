#pragma once
/// @file
/// SVG morphology compute program expressed in the shader IR.
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/MorphologyBindings.h"
namespace donner::gpu::shader::programs {
/**
 * Computes component-wise erosion or dilation with transparent pixels outside the source.
 * Hosts pass nonnegative radii no greater than 31 per axis and operation 0 (erode) or 1 (dilate).
 * Zero radii copy the source. Larger kernels are decomposed by the host into bounded passes.
 * Source and destination have equal extents; excess destination invocations return without writing.
 */
ShaderResult<IrModule> BuildMorphologyModule();
}  // namespace donner::gpu::shader::programs
