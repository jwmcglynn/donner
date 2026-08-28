#pragma once
/// @file
/// The color-matrix compute program, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader::programs {

/// Bind group indices of the color-matrix compute program, shared by the IR builder and every
/// host that creates its bind group layout.
enum class ColorMatrixBinding : uint32_t {
  InputTexture = 0,   //!< Sampled `texture_2d<f32>` source.
  OutputTexture = 1,  //!< `texture_storage_2d<rgba8unorm, write>` destination.
  Params = 2,         //!< Uniform buffer holding five vec4f: the four
                      //!< per-input-channel multiplier columns and the constant
                      //!< offset column added after them.
  Bias = 3,           //!< Read-only storage buffer of per-checker bias vectors.
};

/// Workgroup size the color-matrix entry point declares. Hosts divide the destination extent by
/// this to compute dispatch counts.
inline constexpr uint32_t kColorMatrixWorkgroupSize = 8;

/// Number of bias vectors the storage buffer must hold.
inline constexpr uint32_t kColorMatrixBiasCount = 2;

/**
 * Builds the color-matrix compute program: one `@compute @workgroup_size(8, 8, 1)` entry point
 * named `cs_main` that reads one texel of a sampled texture, multiplies it by a 4x4 matrix from
 * a uniform buffer, adds a bias vector selected per texel from a read-only storage buffer,
 * saturates the result, and writes it to a write-only storage texture.
 *
 * Invocations outside the destination extent return without writing, so a dispatch rounded up to
 * whole workgroups is safe.
 */
ShaderResult<IrModule> BuildColorMatrixModule();

}  // namespace donner::gpu::shader::programs
