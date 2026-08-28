#pragma once
/// @file
/// The subregion-clip compute program, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader::programs {

/// Bind group indices of the subregion-clip compute program, shared by the IR builder and every
/// host that creates its bind group layout.
enum class SubregionClipBinding : uint32_t {
  InputTexture = 0,   //!< Sampled `texture_2d<f32>` source.
  OutputTexture = 1,  //!< `texture_storage_2d<rgba8unorm, write>` destination.
  Params = 2,         //!< Uniform buffer holding the inverse transform and the user-space
                      //!< rectangle to keep.
};

/// Workgroup size the subregion-clip entry point declares. Hosts divide the destination extent by
/// this to compute dispatch counts.
inline constexpr uint32_t kSubregionClipWorkgroupSize = 8;

/**
 * Builds the subregion-clip compute program: one `@compute @workgroup_size(8, 8, 1)` entry point
 * named `cs_main` that copies texels inside a rectangle and writes transparent black outside it.
 *
 * The rectangle is axis-aligned in user space, not in the destination's pixel space, so the test
 * runs on the pixel center mapped back through the uniform's inverse transform. That is what
 * makes the clip correct under a rotated transform, where a pixel-space rectangle would not
 * describe the same region. The rectangle is half-open on both axes: a coordinate equal to the
 * low edge is inside, one equal to the high edge is outside, so abutting subregions neither
 * overlap nor leave a seam.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildSubregionClipModule();

}  // namespace donner::gpu::shader::programs
