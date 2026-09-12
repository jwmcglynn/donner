#pragma once
/// @file
/// The deterministic Metal argument-table mapping shared by the MSL emitter and the Metal
/// backend.
///
/// Bind group 0 uses one flat argument table per stage. Slot 0 carries exact declared buffer
/// lengths and buffer binding `b` uses slot `1 + b`. Vertex slot zero uses index 30; additional
/// active vertex slots use unoccupied vertex-stage indices chosen from the pipeline layout.
/// Texture and sampler bindings use their binding numbers directly.

#include <cstdint>

namespace donner::gpu::shader {

/// Reserved Metal buffer index for the fixed table of declared buffer lengths. Populated from
/// every buffer binding in a group, uniform bindings included, and uploaded to each stage of the
/// active encoder whenever the bound group changes. Raw MSL this backend accepts must leave this
/// index free.
inline constexpr uint32_t kMslBufferLengthsIndex = 0;
/// Number of buffer bindings before the dedicated vertex slot.
inline constexpr uint32_t kMslBufferBindingCount = 29;
/// Number of texture argument slots per stage.
inline constexpr uint32_t kMslTextureBindingCount = 128;
/// Number of sampler argument slots per stage.
inline constexpr uint32_t kMslSamplerBindingCount = 16;
/// Highest Metal buffer index, reserved for vertex slot zero. Additional vertex slots use
/// lower indices not occupied by vertex-visible resource bindings in the active pipeline.
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
