#pragma once
/// @file
/// Typed drop-shadow offset, flood, and source-over composition.
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/DropShadowBindings.h"
namespace donner::gpu::shader::programs {
/**
 * Composes an offset blurred alpha below the original source using a straight-alpha flood color.
 * Offsets are finite pixels in [-4096,4096], rounded half away from zero like the software path.
 * The original source and output extents match. Samples outside the blurred source are transparent.
 */
ShaderResult<IrModule> BuildDropShadowModule();
}  // namespace donner::gpu::shader::programs
