#pragma once
/// @file
/// SVG diffuse and specular lighting compute programs expressed in the shader IR.

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

/**
 * Builds Phong specular lighting over the input alpha height map.
 *
 * The program shares light-source, normal-reconstruction, and sampling behavior with the diffuse
 * program and writes the largest color channel as alpha.
 */
ShaderResult<IrModule> BuildSpecularLightingModule();

}  // namespace donner::gpu::shader::programs
