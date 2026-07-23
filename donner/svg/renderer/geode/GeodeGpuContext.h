#pragma once
/// @file
/// \c donner::geode::GeodeGpuContext - the \c donner::gpu facing context Geode's encoders record
/// against, plus \c donner::geode::GeodeTransientResources, the per-recording RAII holder for
/// transient GPU handles.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "donner/gpu/GpuResult.h"
#include "donner/gpu/Handles.h"

namespace donner::gpu {
class Device;
}  // namespace donner::gpu

namespace donner::geode {

class GeodeDevice;
class GeodeMaskPipeline;

/**
 * The shared identity resources every \c GeoEncoder bind group references: the 1x1 dummy
 * pattern / clip-mask textures (plus views and samplers) bound into inactive pattern and
 * clip-mask slots, and the one-element identity instance-transform storage buffer bound at
 * binding 7 of non-instanced Slug fills.
 *
 * Created once per \c gpu::Device via \ref Create. Production calls it from
 * `GeodeDevice::initSharedResources` against the device's adapter; the GPU-less
 * RecordingDevice test harness calls it directly so captured command streams carry the exact
 * production labels and payloads. Defined in GeodeDevice.cc.
 */
struct GeodeSharedGpuResources {
  gpu::Texture dummyPatternTexture;           //!< 1x1 opaque-black RGBA8 pattern dummy.
  gpu::TextureView dummyPatternTextureView;   //!< View over \ref dummyPatternTexture.
  gpu::Sampler dummyPatternSampler;           //!< Linear-Repeat pattern sampler.
  gpu::Texture dummyClipMaskTexture;          //!< 1x1 full-coverage RGBA8 clip-mask dummy.
  gpu::TextureView dummyClipMaskTextureView;  //!< View over \ref dummyClipMaskTexture.
  gpu::Sampler dummyClipMaskSampler;          //!< Linear-ClampToEdge clip-mask sampler.
  /// 32-byte identity instance-transform buffer (two vec4f rows: `{(1,0,0,0), (0,1,0,0)}`).
  gpu::Buffer identityInstanceTransformBuffer;

  /**
   * Creates the shared resources on \p gpuDevice, including the 1x1 texture uploads and the
   * identity-buffer write. Returns the first creation/write error unchanged.
   *
   * @param gpuDevice The device the resources are created on; must outlive the result.
   */
  static gpu::Result<GeodeSharedGpuResources> Create(gpu::Device& gpuDevice);
};

/**
 * Everything \c GeoEncoder and \c GeodeTextureEncoder need to record a frame through the
 * \c donner::gpu runtime.
 *
 * Production wires one of these from \c GeodeDevice (see `GeodeDevice::gpuContext()`): the
 * `gpuDevice` is the device's \c GeodeWgpuAdapterDevice, the dummy resources are the shared
 * device-owned identity fills for inactive pattern / clip-mask bind slots, and the counter
 * helpers forward to the \c GeodeDevice perf counters. A test harness (the RecordingDevice
 * suite) can instead construct one against a GPU-less \c donner::gpu::Device with
 * `geodeDevice == nullptr` (counters become no-ops) and `maskPipelineOverride` set.
 *
 * Non-owning: every pointer references state owned by the wiring scope, which must outlive any
 * encoder recording against this context.
 */
struct GeodeGpuContext {
  /// The GPU runtime device recording is performed against. Required (never null in a wired
  /// context).
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
  /// One-element identity instance-transform storage buffer (32 bytes) bound at binding 7 of
  /// non-instanced Slug fills.
  const gpu::Buffer* identityInstanceTransformBuffer = nullptr;

  /// Test override for \ref maskPipeline. Production leaves this null and resolves the shared
  /// lazily-built pipeline through \ref geodeDevice.
  GeodeMaskPipeline* maskPipelineOverride = nullptr;

  /// Live resident-geometry byte gauge shared with `GeodeResidentSlot`s. May be null in tests.
  std::shared_ptr<std::atomic<int64_t>> residentBytesGauge;

  // Counter helpers: exact forwarders to the GeodeDevice counter hooks so production counter
  // behavior is unchanged by the RHI migration; cheap no-ops when `geodeDevice` is null.
  // Defined in GeodeDevice.cc (GeodeDevice is incomplete here).

  /// Forwards to `GeodeDevice::countBuffer` when wired.
  void countBuffer() const;
  /// Forwards to `GeodeDevice::countTexture` when wired.
  void countTexture() const;
  /// Forwards to `GeodeDevice::countBindGroup` when wired.
  void countBindGroup() const;
  /// Forwards to `GeodeDevice::countDraw` when wired.
  void countDraw() const;
  /// Forwards to `GeodeDevice::countPipelineSwitch` when wired.
  void countPipelineSwitch() const;
  /// Forwards to `GeodeDevice::countPathEncode` when wired.
  void countPathEncode() const;
  /// Forwards to `GeodeDevice::countBufferWrite` when wired. @param bytes Payload byte count.
  void countBufferWrite(uint64_t bytes) const;
  /// Forwards to `GeodeDevice::countTextureWrite` when wired. @param bytes Payload byte count.
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
 * until the frame's `gpu::Device::submit` returns: submit re-validates every recorded resource
 * identity and fails closed on a destroyed handle, so this keepalive is mandatory-by-validation.
 *
 * The wgpu-era equivalent (`ScopedWgpuResourceArena` in GeodeWgpuUtil.h) remains in use by the
 * not-yet-migrated filter / editor code paths only.
 */
struct GeodeTransientResources {
  std::vector<gpu::Buffer> buffers;        //!< Retained transient buffers.
  std::vector<gpu::Texture> textures;      //!< Retained transient textures.
  std::vector<gpu::TextureView> views;     //!< Retained transient texture views.
  std::vector<gpu::Sampler> samplers;      //!< Retained transient samplers.
  std::vector<gpu::BindGroup> bindGroups;  //!< Retained transient bind groups.

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
