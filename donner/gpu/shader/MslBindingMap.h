#pragma once
/// @file
/// The deterministic Metal argument-table mapping shared by the MSL emitter and the Metal
/// backend.
///
/// Both sides of the Metal path must agree on how RHI (group, binding) pairs map onto Metal's
/// per-stage argument tables. This header is the single source of truth (design 0053 "Original
/// emitters": all emitters consume the same binding metadata):
///
/// - Uniform and storage buffer binding `b` -> `[[buffer(1 + b)]]`.
/// - Texture binding `b` -> `[[texture(b)]]`.
/// - Sampler binding `b` -> `[[sampler(b)]]`.
/// - Stage-in vertex data occupies the dedicated vertex buffer index 30 (the last slot of
///   Metal's 0..30 vertex buffer argument table). The `1 + b` mapping therefore supports RHI
///   buffer bindings b in 0..28 only: b = 29 would land on the reserved index 30, and b >= 30
///   would exceed Metal's table. The MSL emitter and the Metal backend both fail closed for
///   b >= 29; the solid-fill family uses b in 0..11.
///
/// Only bind group 0 exists in the solid-fill pipeline family; multi-group support would extend
/// this map with a per-group base offset when a pipeline family needs it.

#include <cstdint>

namespace donner::gpu::shader {

/// Metal buffer argument-table index for an RHI buffer binding (uniform or storage).
/// @param binding RHI binding index within group 0.
inline constexpr uint32_t MslBufferIndex(uint32_t binding) {
  return 1 + binding;
}

/// Metal texture argument-table index for an RHI texture binding.
/// @param binding RHI binding index within group 0.
inline constexpr uint32_t MslTextureIndex(uint32_t binding) {
  return binding;
}

/// Metal sampler argument-table index for an RHI sampler binding.
/// @param binding RHI binding index within group 0.
inline constexpr uint32_t MslSamplerIndex(uint32_t binding) {
  return binding;
}

/// Dedicated Metal vertex buffer index for stage-in vertex data (see file comment).
inline constexpr uint32_t kMslVertexBufferIndex = 30;

}  // namespace donner::gpu::shader
