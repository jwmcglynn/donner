#pragma once
/// @file
/// SVG diffuse lighting compute program expressed in the shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/LightingBindings.h"

namespace donner::gpu::shader::programs {

/**
 * Builds Lambertian diffuse lighting over the input alpha height map.
 *
 * The program supports distant, point, and spot lights, including transformed spotlight cones
 * and independently bounded sampling subregions.
 */
ShaderResult<IrModule> BuildDiffuseLightingModule();

}  // namespace donner::gpu::shader::programs
