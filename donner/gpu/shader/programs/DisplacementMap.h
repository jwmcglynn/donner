#pragma once
/// @file
/// Typed channel-driven displacement with transparent-border bilinear sampling.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/DisplacementMapBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds a displacement compute program.
 *
 * Inputs and output have matching extents. The map is premultiplied RGBA; RGB selectors recover
 * straight color while the alpha selector reads alpha directly. Source samples beyond the input
 * bounds are transparent.
 */
ShaderResult<IrModule> BuildDisplacementMapModule();

}  // namespace donner::gpu::shader::programs
