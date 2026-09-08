#pragma once
/// @file
/// The deterministic Metal argument-table mapping shared by the MSL emitter and the Metal
/// backend.
///
/// Bind group 0 uses one flat argument table per stage. Slot 0 carries exact declared buffer
/// lengths, buffer binding `b` uses slot `1 + b`, and vertex data uses slot 30. Texture and
/// sampler bindings use their binding numbers directly.

#include <cstdint>

namespace donner::gpu::shader {

/// Fixed table of declared buffer lengths, copied when a storage-buffer group is bound.
inline constexpr uint32_t kMslBufferLengthsIndex = 0;
/// Number of buffer bindings before the dedicated vertex slot.
inline constexpr uint32_t kMslBufferBindingCount = 29;
/// Number of texture argument slots per stage.
inline constexpr uint32_t kMslTextureBindingCount = 128;
/// Number of sampler argument slots per stage.
inline constexpr uint32_t kMslSamplerBindingCount = 16;
/// Dedicated Metal vertex buffer index for stage-in vertex data.
inline constexpr uint32_t kMslVertexBufferIndex = 30;

/// Metal buffer argument-table index for an RHI buffer binding (uniform or storage).
/// Invalid bindings return the reserved vertex index so callers reject them without overflow.
/// @param binding RHI binding index within group 0.
inline constexpr uint32_t MslBufferIndex(uint32_t binding) {
  return binding < kMslBufferBindingCount ? 1 + binding : kMslVertexBufferIndex;
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

}  // namespace donner::gpu::shader
