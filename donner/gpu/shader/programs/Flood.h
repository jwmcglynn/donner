#pragma once
/// @file
/// The flood compute program, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader::programs {

/// Bind group indices of the flood compute program, shared by the IR builder and every host that
/// creates its bind group layout.
enum class FloodBinding : uint32_t {
  OutputTexture = 0,  //!< `texture_storage_2d<rgba8unorm, write>` destination.
  Params = 1,         //!< Uniform buffer holding the single color the destination is filled with.
};

/// Workgroup size the flood entry point declares. Hosts divide the destination extent by this to
/// compute dispatch counts.
inline constexpr uint32_t kFloodWorkgroupSize = 8;

/**
 * Builds the flood compute program: one `@compute @workgroup_size(8, 8, 1)` entry point named
 * `cs_main` that writes one uniform color to every texel of a write-only storage texture.
 *
 * The program reads no texture, so the destination extent is the only thing that bounds it. The
 * color is written exactly as the uniform carries it: whether it is premultiplied is the caller's
 * decision, and nothing here re-associates it with the alpha channel.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildFloodModule();

}  // namespace donner::gpu::shader::programs
