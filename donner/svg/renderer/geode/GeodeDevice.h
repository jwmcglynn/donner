#pragma once
/// @file
/// RAII wrapper around a WebGPU device — headless or host-provided.

#include <memory>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/svg/renderer/geode/GeodeCounters.h"

namespace donner::geode {

/**
 * Configuration for embedding Geode into a host application that already owns a
 * WebGPU device.
 *
 * The host is responsible for the lifetime of the device and queue — they must
 * remain valid for the entire lifetime of any `GeodeDevice` or `RendererGeode`
 * constructed from this config.
 *
 * Example:
 * @code
 *   GeodeEmbedConfig config;
 *   config.device = myDevice;
 *   config.queue = myQueue;
 *   config.textureFormat = wgpu::TextureFormat::BGRA8Unorm;
 *
 *   auto geodeDevice = GeodeDevice::CreateFromExternal(config);
 *   RendererGeode renderer(std::move(geodeDevice));
 * @endcode
 */
struct GeodeEmbedConfig {
  /// Host-provided WebGPU device. Must not be null.
  wgpu::Device device;

  /// Host-provided queue associated with `device`. Must not be null.
  wgpu::Queue queue;

  /// Texture format for render targets. Must match the format of any texture
  /// passed to `RendererGeode::setTargetTexture()`.
  wgpu::TextureFormat textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  /// Optional adapter handle. When provided, Geode uses it for hardware
  /// workaround detection (e.g., Intel Arc + Vulkan alpha-coverage fallback).
  /// When null, workaround detection is skipped — the host is assumed to
  /// know its own hardware characteristics.
  wgpu::Adapter adapter;
};

/**
 * Owns (or wraps) a WebGPU device/queue pair for GPU rendering.
 *
 * GeodeDevice is the entry point to the Geode rendering backend. In **headless
 * mode** (`CreateHeadless`), it creates a WebGPU instance, selects a default
 * adapter, and creates a device — all without any window system integration.
 *
 * In **embedded mode** (`CreateFromExternal`), it wraps a device and queue
 * already created by the host application. The host retains ownership of the
 * underlying WebGPU objects; GeodeDevice's destructor will not destroy them.
 *
 * Typical headless usage:
 *
 *     auto maybeDevice = GeodeDevice::CreateHeadless();
 *     if (!maybeDevice) {
 *       // No GPU available.
 *       return;
 *     }
 *
 * Typical embedded usage:
 *
 *     GeodeEmbedConfig config;
 *     config.device = hostDevice;
 *     config.queue = hostQueue;
 *     auto geodeDevice = GeodeDevice::CreateFromExternal(config);
 */
class GeodeDevice {
public:
  /**
   * Create a headless GeodeDevice.
   *
   * @return A valid GeodeDevice on success, or an empty unique_ptr if the
   *   runtime could not create an adapter/device (e.g., no GPU, no driver).
   */
  static std::unique_ptr<GeodeDevice> CreateHeadless();

  /**
   * Create a GeodeDevice wrapping a host-provided device and queue.
   *
   * The returned device does NOT own the underlying WebGPU instance, adapter,
   * device, or queue — the host is responsible for keeping them alive.
   *
   * @param config Embedding configuration with valid device/queue handles.
   * @return A valid GeodeDevice on success, or null if \p config.device or
   *   \p config.queue is null.
   */
  static std::unique_ptr<GeodeDevice> CreateFromExternal(const GeodeEmbedConfig& config);

  /// Destructor releases the device and all GPU resources.
  ~GeodeDevice();

  // Non-copyable, movable.
  GeodeDevice(const GeodeDevice&) = delete;
  GeodeDevice& operator=(const GeodeDevice&) = delete;
  /// Move constructor.
  GeodeDevice(GeodeDevice&&) noexcept;
  /// Move assignment operator.
  GeodeDevice& operator=(GeodeDevice&&) noexcept;

  /// Returns the wgpu::Device. Guaranteed valid for the lifetime of this object.
  const wgpu::Device& device() const { return device_; }

  /// Returns the default queue.
  const wgpu::Queue& queue() const { return queue_; }

  /// Returns the adapter backing this device. May be null in embedded mode if
  /// the host did not provide an adapter.
  const wgpu::Adapter& adapter() const { return adapter_; }

  /// Render-target texture format. Defaults to RGBA8Unorm for headless devices;
  /// set by the host via `GeodeEmbedConfig::textureFormat` in embedded mode.
  wgpu::TextureFormat textureFormat() const { return textureFormat_; }

  /**
   * Enqueue a GPU buffer for deferred destruction. The buffer handle is kept
   * alive until `drainDeferredDestroys()` is called, preventing the underlying
   * GPU resource from being freed while an in-flight command buffer may still
   * reference it.
   */
  void deferDestroy(wgpu::Buffer buffer);

  /**
   * Enqueue a GPU texture for deferred destruction. Same semantics as the
   * buffer variant.
   */
  void deferDestroy(wgpu::Texture texture);

  /**
   * Drop all deferred-destroy handles, releasing their GPU resources.
   *
   * Called at the top of each frame (before new allocations) so resources
   * from the previous frame's command buffer submission have had time to
   * complete on the GPU. WebGPU internally reference-counts resources used
   * by submitted command buffers, so dropping our handle here is safe even
   * without an explicit `device.poll()`.
   */
  void drainDeferredDestroys();

  /**
   * Whether to use alpha-coverage AA instead of hardware 4× MSAA with
   * `@builtin(sample_mask)`.
   *
   * Returns true on Intel + Vulkan, where writing `@builtin(sample_mask)`
   * from overlapping band quads hangs Mesa ANV / Xe KMD (observed on
   * Arc A380, Mesa 25.2.8). The alpha-coverage path folds coverage
   * into the fragment color instead of relying on the hardware sample
   * mask, and runs at `sampleCount() == 1` (no MSAA / no resolve) to
   * avoid a second class of flaky MSAA-resolve hangs on the same driver.
   */
  bool useAlphaCoverageAA() const { return useAlphaCoverageAA_; }

  /**
   * MSAA sample count for render pipelines and render-target textures.
   *
   * Returns 1 on the alpha-coverage path (Intel Arc + Vulkan), where
   * MSAA resolve triggers flaky GPU hangs on Mesa ANV / Xe KMD. All
   * other adapters get 4× MSAA with hardware sample-mask AA.
   */
  uint32_t sampleCount() const { return useAlphaCoverageAA_ ? 1u : 4u; }

  /**
   * Install a `GeodeCounters` struct for this device. Non-owning; the
   * caller must keep the struct alive for as long as the device might
   * increment it. Pass `nullptr` to disable instrumentation.
   *
   * See design doc 0030 (geode_performance). All Geode components that
   * hold a `GeodeDevice&` route their per-frame hot-path allocation and
   * submission sites through this hook.
   */
  void setCounters(GeodeCounters* counters) { counters_ = counters; }

  /// Non-owning pointer to the installed counters, or null.
  GeodeCounters* counters() const { return counters_; }

  // Counter increment helpers. Cheap no-op when counters are disabled.
  void countBuffer() const {
    if (counters_) ++counters_->bufferCreates;
  }
  void countBindGroup() const {
    if (counters_) ++counters_->bindgroupCreates;
  }
  void countTexture() const {
    if (counters_) ++counters_->textureCreates;
  }
  void countSubmit() const {
    if (counters_) ++counters_->submits;
  }
  void countPathEncode() const {
    if (counters_) ++counters_->pathEncodes;
  }
  void countDraw() const {
    if (counters_) ++counters_->drawCalls;
  }
  void countPipelineSwitch() const {
    if (counters_) ++counters_->pipelineSwitches;
  }

  /**
   * Whether the driver supports GPU timestamp queries. Always false
   * today — reserved for future work (design doc 0030, "Future Work").
   */
  bool supportsTimestamps() const { return false; }

  /// @name Shared dummy resources (design doc 0030 Milestone 4.2)
  /// @{
  ///
  /// GeoEncoder's bind groups always include pattern + clip-mask
  /// texture/sampler slots, even when the current draw doesn't
  /// actually use them. Each slot is filled with a 1×1 "identity"
  /// texture when the feature is inactive. Prior to M4.2 every
  /// GeoEncoder instance created its own dummies (two textures per
  /// encoder), which showed up as 2+ `textureCreates` per frame per
  /// push/pop. Caching the dummies on the device — one instance per
  /// GeodeDevice — drops that to zero steady-state.

  /// 1×1 opaque-black RGBA8 dummy. Bound into the pattern slot when
  /// the current draw is solid / gradient (not a pattern). The shader
  /// does not sample from it, but the bind group layout still requires
  /// a valid binding.
  const wgpu::Texture& dummyPatternTexture() const;
  /// View of `dummyPatternTexture()`.
  const wgpu::TextureView& dummyPatternTextureView() const;
  /// Linear-Repeat sampler used for both the dummy and real pattern tiles.
  const wgpu::Sampler& dummyPatternSampler() const;

  /// 1×1 R8Unorm with value `0xFF` (= 1.0 coverage). Bound into the
  /// clip-mask slot when no clip mask is active — the shader
  /// multiplies coverage by this value, so `1.0` is a no-op.
  const wgpu::Texture& dummyClipMaskTexture() const;
  /// View of `dummyClipMaskTexture()`.
  const wgpu::TextureView& dummyClipMaskTextureView() const;
  /// Linear-ClampToEdge sampler used for both the dummy and real clip masks.
  const wgpu::Sampler& dummyClipMaskSampler() const;

  /// One-element instance-transform storage buffer carrying the identity
  /// affine. Bound at binding 7 of the Slug fill bind-group layout by
  /// every non-instanced solid fill so the bind-group layout stays
  /// stable across draw calls regardless of whether `fillPathInstanced`
  /// is in play. See design doc 0030 §M6 Bullet 2.
  ///
  /// Layout mirrors the WGSL `InstanceTransform` struct in
  /// `shaders/slug_fill.wgsl`: two `vec4f` per entry, row-major affine,
  /// so the identity is `{(1, 0, 0, 0), (0, 1, 0, 0)}`. The `.z`
  /// components carry the translation (0 for identity).
  const wgpu::Buffer& identityInstanceTransformBuffer() const;
  /// @}

private:
  GeodeDevice();

  // Order matters: queue/device/adapter/instance destroy bottom-up.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Exposed shortcuts to Impl members (for fast access without indirection).
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::TextureFormat textureFormat_ = wgpu::TextureFormat::RGBA8Unorm;

  bool useAlphaCoverageAA_ = false;
  bool supportsTimestamps_ = false;

  /// True when this device was created via CreateFromExternal(). The destructor
  /// skips releasing the instance/adapter since the host owns them.
  bool external_ = false;

  GeodeCounters* counters_ = nullptr;

  // Deferred-destroy queues: resources enqueued via deferDestroy() are held
  // alive until drainDeferredDestroys() drops them at the next frame boundary.
  std::vector<wgpu::Buffer> pendingBuffers_;
  std::vector<wgpu::Texture> pendingTextures_;
};

}  // namespace donner::geode
