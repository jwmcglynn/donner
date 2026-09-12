#pragma once
/// @file
/// SVG tile compute program expressed in the shader IR.
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/TileBindings.h"
namespace donner::gpu::shader::programs {
/**
 * Repeats a source rectangle across the destination, with positive wrapping for negative origins.
 * Degenerate rectangles produce transparent black. Samples clamp to the source texture extent.
 * Hosts bound origins and extents to the filter coordinate range; intermediate integer sums
 * must fit signed 32-bit coordinates. Destination edge invocations return without writing.
 */
ShaderResult<IrModule> BuildTileModule();
}  // namespace donner::gpu::shader::programs
