#pragma once
/// @file
/// SVG Gaussian blur compute program expressed in the shader IR.
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/GaussianBlurBindings.h"
namespace donner::gpu::shader::programs {
/**
 * Runs one Gaussian or box-blur pass with transparent, duplicate, or wrap sampling.
 * The host bounds finite sigma and box extents through filter admission, and supplies equally
 * sized source/destination textures. Gaussian support is capped at127 pixels. The optional
 * output clip is applied after sampling, before the shared [0,1] output clamp.
 */
ShaderResult<IrModule> BuildGaussianBlurModule();
}  // namespace donner::gpu::shader::programs
