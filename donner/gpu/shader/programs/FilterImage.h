#pragma once
/// @file
/// SVG image filter compute program expressed in the shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/FilterImageBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds affine image placement with nearest-neighbor, CSS pixelated, and
 * Mitchell-Netravali sampling.
 *
 * Source taps clamp to the source edge, while output pixel centers outside the image remain
 * transparent. Results are clamped to finite premultiplied color inputs' valid range.
 */
ShaderResult<IrModule> BuildFilterImageModule();

}  // namespace donner::gpu::shader::programs
