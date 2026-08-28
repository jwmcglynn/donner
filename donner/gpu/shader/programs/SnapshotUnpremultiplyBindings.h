#pragma once
/// @file
/// Bind group indices of the snapshot-unpremultiply program.
///
/// Split out from the program header so a host can build the program's bind group layout without
/// linking the shader IR. The pipeline that does so consumes build-time emitted WGSL, and pulling
/// in the IR for an enum would put the emitters back in binaries that exist to avoid them.

#include <cstdint>

namespace donner::gpu::shader::programs {

/// Bind group indices of the snapshot-unpremultiply program, shared by the IR builder and every
/// host that creates its bind group layout.
enum class SnapshotUnpremultiplyBinding : uint32_t {
  InputTexture = 0,   //!< Sampled `texture_2d<f32>` premultiplied render target.
  OutputTexture = 1,  //!< `texture_storage_2d<rgba8unorm, write>` straight-alpha destination.
};

}  // namespace donner::gpu::shader::programs
