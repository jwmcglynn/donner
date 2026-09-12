#pragma once
/// @file
/// Typed SVG Perlin turbulence and fractal-noise program.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/TurbulenceBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the turbulence compute program.
 *
 * The host supplies bounded noise parameters and deterministic permutation/gradient tables.
 * Integer pixel coordinates are transformed to filter space, evaluated independently for four
 * channels, and written as premultiplied RGBA.
 */
ShaderResult<IrModule> BuildTurbulenceModule();

}  // namespace donner::gpu::shader::programs
