#pragma once
/// @file
/// Documented validation limits enforced by \c donner::gpu::Device.
///
/// Limits exist so that untrusted SVG-derived dimensions (geometry volume, image sizes,
/// render-target extents) fail deterministically with \ref GpuErrorType::LimitExceeded instead of
/// exhausting a driver (design 0053 "Security and Reliability").

#include <cstdint>

namespace donner::gpu {

/// Maximum texture width or height in texels.
inline constexpr uint32_t kMaxTextureDimension = 16384;

/// Maximum buffer size in bytes (1 GiB).
inline constexpr uint64_t kMaxBufferByteSize = uint64_t(1) << 30;

/// Maximum number of entries in one bind group layout, and the exclusive upper bound for binding
/// indices.
inline constexpr uint32_t kMaxBindings = 32;

/// Maximum number of bind groups in a pipeline layout, and the exclusive upper bound for
/// `setBindGroup` indices.
inline constexpr uint32_t kMaxBindGroups = 4;

/// Maximum number of vertex buffer layouts in a render pipeline, and the exclusive upper bound
/// for `setVertexBuffer` slots.
inline constexpr uint32_t kMaxVertexBuffers = 8;

/// Maximum total vertex attributes in a render pipeline, and the exclusive upper bound for
/// attribute shader locations. 16 is the strictest common cap across the native APIs this
/// runtime targets (well below Metal's 31 per-stage limit).
inline constexpr uint32_t kMaxVertexAttributes = 16;

/// Maximum number of color attachments in a render pass or render pipeline.
inline constexpr uint32_t kMaxColorAttachments = 4;

/// Required alignment of \ref TexelCopyBufferLayout::bytesPerRow. 256 is the strictest row-pitch
/// alignment across the native APIs this runtime targets, so enforcing it uniformly keeps copies
/// portable without per-backend repacking. The same portability reasoning requires
/// `TexelCopyBufferLayout::offsetBytes` to be aligned to the copied format's texel size.
inline constexpr uint32_t kTexelRowPitchAlignment = 256;

}  // namespace donner::gpu
