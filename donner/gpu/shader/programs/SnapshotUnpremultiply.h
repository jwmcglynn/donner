#pragma once
/// @file
/// The snapshot-unpremultiply compute program, expressed in the \c donner::gpu::shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiplyBindings.h"

namespace donner::gpu::shader::programs {

/// Workgroup size the entry point declares. Hosts divide the destination extent by this to
/// compute dispatch counts.
inline constexpr uint32_t kSnapshotUnpremultiplyWorkgroupSize = 8;

/**
 * Builds the snapshot-unpremultiply compute program: one
 * `@compute @workgroup_size(8, 8, 1)` entry point named `cs_main` that reads one texel of the
 * premultiplied render target and writes the straight-alpha texel to a write-only storage
 * texture.
 *
 * The arithmetic reproduces the CPU reference loop byte for byte:
 *
 *     a == 0  ->  (0, 0, 0, 0)
 *     a != 0  ->  min(255, (premul * 255 + a / 2) / a)     (round half up)
 *
 * It is integer arithmetic on the recovered 8-bit channels rather than float division, because
 * the readback's contract is that the GPU and CPU paths produce identical bytes, and float
 * division would agree only to within a rounding mode.
 *
 * Recovering those channels multiplies the normalized texel by 255 and rounds. The three
 * backends spell rounding slightly differently - half to even in WGSL and SPIR-V, half away from
 * zero in MSL - and they agree here because a unorm8 texel times 255 lands exactly on an integer
 * in f32, so no value is ever at a half-way tie.
 *
 * The destination extent is the smaller of the two textures. Clamping to both matters because
 * the staging texture is pooled: writing only the region the input covers would leave the rest
 * of a larger pooled texture holding a previous snapshot's pixels instead of raising a
 * validation error. Invocations outside that extent return without writing, so a dispatch
 * rounded up to whole workgroups is safe.
 */
ShaderResult<IrModule> BuildSnapshotUnpremultiplyModule();

}  // namespace donner::gpu::shader::programs
