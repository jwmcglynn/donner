#pragma once
/// @file
/// \c donner::geode::GeodeGpuContext - the \c donner::gpu facing context Geode's encoders record
/// against, plus \c donner::geode::GeodeTransientResources, the per-recording RAII holder for
/// transient GPU handles.

#include <cstdint>
#include <deque>
#include <utility>

#include "donner/gpu/Handles.h"

namespace donner::gpu {
class Device;
}  // namespace donner::gpu

namespace donner::geode {

class GeodeDevice;
class GeodeMaskPipeline;

/// Uniform-buffer binding offset alignment every Geode uniform arena honors. 256 is the WebGPU
/// default minUniformBufferOffsetAlignment; a path that ever queries the device limit instead must
/// keep this in sync, or its bindings fail validation on a device with a larger one.
///
/// Defined here rather than beside any one arena because four of them need it, and a per-arena
/// copy is how they drift apart.
inline constexpr uint64_t kUniformOffsetAlignment = 256u;

/**
 * Everything Geode's encoders need to record a frame through the \c donner::gpu runtime.
 *
 * Production wires one of these from \c GeodeDevice (see `GeodeDevice::gpuContext()`): the
 * `gpuDevice` is the device's adapter, the dummy resources are the shared device-owned identity
 * fills for inactive pattern / clip-mask / paint bind slots, and the counter helpers forward to
 * the \c GeodeDevice perf counters. A test harness can instead construct one against a GPU-less
 * \c donner::gpu::Device with `geodeDevice == nullptr` (counters become no-ops) and
 * `maskPipelineOverride` set.
 *
 * Non-owning: every pointer references state owned by the wiring scope, which must outlive any
 * encoder recording against this context.
 */
struct GeodeGpuContext {
  /// The GPU runtime device recording is performed against. Required (never null when wired).
  gpu::Device* gpuDevice = nullptr;

  /// Optional counter sink + lazy mask-pipeline owner. Null in GPU-less test harnesses.
  GeodeDevice* geodeDevice = nullptr;

  /// 1x1 opaque-black RGBA8 dummy view bound into the pattern slot of solid / gradient draws.
  const gpu::TextureView* dummyPatternTextureView = nullptr;
  /// Linear-Repeat sampler paired with \ref dummyPatternTextureView (and real pattern tiles).
  const gpu::Sampler* dummyPatternSampler = nullptr;
  /// 1x1 full-coverage dummy view bound into the clip-mask slot when no clip mask is active.
  const gpu::TextureView* dummyClipMaskTextureView = nullptr;
  /// Linear-ClampToEdge sampler paired with \ref dummyClipMaskTextureView (and real masks).
  const gpu::Sampler* dummyClipMaskSampler = nullptr;
  /// One full-size identity instance record, bound by every non-instanced Slug fill.
  const gpu::Buffer* identityInstanceRecordBuffer = nullptr;
  /// One zero-filled gradient paint block, bound by draws that carry no gradient paint.
  const gpu::Buffer* dummyPaintDataBuffer = nullptr;

  /// Test override for \ref maskPipeline. Production leaves this null and resolves the shared
  /// lazily-built pipeline through \ref geodeDevice.
  GeodeMaskPipeline* maskPipelineOverride = nullptr;

  // Counter helpers: exact forwarders to the GeodeDevice counter hooks so counter behavior is
  // unchanged by the migration; cheap no-ops when `geodeDevice` is null. Defined in
  // GeodeDevice.cc, where GeodeDevice is complete.

  /// Forwards to `GeodeDevice::countBuffer` when wired.
  void countBuffer() const;
  /// Forwards to `GeodeDevice::countTexture` when wired.
  void countTexture() const;
  /// Forwards to `GeodeDevice::countBindGroup` when wired.
  void countBindGroup() const;
  /// Forwards to `GeodeDevice::countPipelineSwitch` when wired.
  void countPipelineSwitch() const;
  /// Forwards to `GeodeDevice::countPathEncode` when wired.
  void countPathEncode() const;
  /// Forwards to `GeodeDevice::countBufferWrite` when wired.
  /// @param bytes Payload byte count.
  void countBufferWrite(uint64_t bytes) const;
  /// Forwards to `GeodeDevice::countTextureWrite` when wired.
  /// @param bytes Payload byte count.
  void countTextureWrite(uint64_t bytes) const;
  /// Forwards to `GeodeDevice::countSubmit` when wired.
  void countSubmit() const;

  /// The Slug mask pipeline: \ref maskPipelineOverride when set (tests), otherwise the shared
  /// lazily-built `GeodeDevice::maskPipeline()`. Requires one of the two to be available.
  GeodeMaskPipeline& maskPipeline() const;
};

/**
 * RAII holder for transient GPU handles created while recording draws - per-draw uniform
 * buffers, texture views, samplers, and bind groups. Everything retained here MUST stay alive
 * until the frame's submit returns: submit re-validates every recorded resource identity and
 * fails closed on a destroyed handle, so this keepalive is mandatory-by-validation.
 */
struct GeodeTransientResources {
  // Deques, not vectors: `retain` hands out references that stay bound until the frame's submit
  // returns, and a vector's reallocation on push_back would dangle every earlier one.
  std::deque<gpu::Buffer> buffers;        //!< Retained transient buffers.
  std::deque<gpu::Texture> textures;      //!< Retained transient textures.
  std::deque<gpu::TextureView> views;     //!< Retained transient texture views.
  std::deque<gpu::Sampler> samplers;      //!< Retained transient samplers.
  std::deque<gpu::BindGroup> bindGroups;  //!< Retained transient bind groups.

  /// Retains \p buffer and returns a reference to it for immediate binding.
  /// @param buffer Buffer to retain.
  const gpu::Buffer& retain(gpu::Buffer&& buffer) {
    buffers.push_back(std::move(buffer));
    return buffers.back();
  }
  /// Retains \p texture and returns a reference to it for immediate binding.
  /// @param texture Texture to retain.
  const gpu::Texture& retain(gpu::Texture&& texture) {
    textures.push_back(std::move(texture));
    return textures.back();
  }
  /// Retains \p view and returns a reference to it for immediate binding.
  /// @param view Texture view to retain.
  const gpu::TextureView& retain(gpu::TextureView&& view) {
    views.push_back(std::move(view));
    return views.back();
  }
  /// Retains \p sampler and returns a reference to it for immediate binding.
  /// @param sampler Sampler to retain.
  const gpu::Sampler& retain(gpu::Sampler&& sampler) {
    samplers.push_back(std::move(sampler));
    return samplers.back();
  }
  /// Retains \p bindGroup and returns a reference to it for immediate binding.
  /// @param bindGroup Bind group to retain.
  const gpu::BindGroup& retain(gpu::BindGroup&& bindGroup) {
    bindGroups.push_back(std::move(bindGroup));
    return bindGroups.back();
  }

  /// Releases every retained handle (RAII destruction through the owning device).
  void clear() {
    bindGroups.clear();
    samplers.clear();
    views.clear();
    textures.clear();
    buffers.clear();
  }
};

}  // namespace donner::geode
