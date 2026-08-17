#pragma once
/// @file
/// RAII wrapper around a WebGPU device - headless or host-provided.

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

/// 256-align a tightly packed row-byte count per the WebGPU
/// texture-to-buffer copy rules. Shared by the device's readback-buffer
/// sizing and the renderer's map-range math; the two MUST agree, because a
/// mapped-range request larger than the buffer returns null rather than
/// raising an error.
constexpr uint32_t AlignReadbackBytesPerRow(uint32_t rowBytes) {
  return (rowBytes + 255u) & ~255u;
}

// Forward declarations - GeodeDevice exposes accessors for the pipeline
// objects it owns (see "Shared render / compute pipelines" section below).
// The full class definitions live in their own headers; including them
// here would pull the entire Slug / filter compilation graph into every
// translation unit that only needs a `GeodeDevice*`.
class GeodeCheckerboardPipeline;
class GeodePipeline;
class GeodeGradientPipeline;
class GeodeImagePipeline;
class GeodeMaskPipeline;
class GeodeFilterEngine;
class GeodeSnapshotReadbackPipeline;

/**
 * Move-only resource set for one GPU snapshot unpremultiply readback: a
 * straight-alpha staging texture, its view, and a map-readable readback
 * buffer. Acquired from and returned to the per-device pool keyed by size,
 * so repeat snapshots at the same dimensions allocate nothing. The pool is
 * bounded with least-recently-used eviction, so a caller walking many
 * distinct sizes (for example a window resize) cannot pin unbounded staging
 * memory for the device's lifetime.
 */
struct SnapshotReadbackResources {
  /// Staging texture the compute pass writes the unpremultiplied result into.
  ScopedWgpuHandle<wgpu::Texture> staging;
  /// View of `staging`, kept with the pooled entry so it is created once.
  ScopedWgpuHandle<wgpu::TextureView> stagingView;
  /// Map-readable buffer the staging texture is copied into.
  ScopedWgpuHandle<wgpu::Buffer> readback;
  /// Pool key: staging texture width in pixels.
  uint32_t width = 0;
  /// Pool key: staging texture height in pixels.
  uint32_t height = 0;

  /// True when any required handle is missing; an empty set cannot be used.
  [[nodiscard]] bool empty() const { return !staging || !stagingView || !readback; }
};

/**
 * Configuration for embedding Geode into a host application that already owns a
 * WebGPU device.
 *
 * The host is responsible for the lifetime of the device and queue - they must
 * remain valid for the entire lifetime of any `GeodeDevice` or `RendererGeode`
 * constructed from this config.
 *
 * Example:
 * @code
 *   GeodeEmbedConfig config;
 *   config.instance = myInstance;  // Optional; enables browser snapshot callbacks.
 *   config.device = myDevice;
 *   config.queue = myQueue;
 *   config.textureFormat = wgpu::TextureFormat::BGRA8Unorm;
 *
 *   auto geodeDevice = GeodeDevice::CreateFromExternal(config);
 *   RendererGeode renderer(std::move(geodeDevice));
 * @endcode
 */
struct GeodeEmbedConfig {
  /// Optional host-provided WebGPU instance. Browser embedders should provide
  /// it so synchronous snapshot readback can wait for map callback completion
  /// through `Instance::waitAny()`.
  wgpu::Instance instance;

  /// Host-provided WebGPU device. Must not be null.
  wgpu::Device device;

  /// Host-provided queue associated with `device`. Must not be null.
  wgpu::Queue queue;

  /// Texture format for render targets. Must match the format of any texture
  /// passed to `RendererGeode::setTargetTexture()`.
  wgpu::TextureFormat textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  /// Optional adapter handle. Preserved for hosts that need to query the
  /// adapter associated with the external device.
  wgpu::Adapter adapter;

  /// Optional shared device-lost flag. Hosts that install their own WebGPU
  /// device-lost callback should have that callback set this flag and pass
  /// the same state here, so a driver-reported loss on the host device and a
  /// bounded-wait timeout inside Geode converge on the same
  /// `GeodeDevice::isDeviceLost()` condition. When null, the GeodeDevice
  /// creates a private flag that only bounded-wait timeouts can set.
  std::shared_ptr<GeodeDeviceLostState> lostState;
};

/**
 * Owns (or wraps) a WebGPU device/queue pair for GPU rendering.
 *
 * GeodeDevice is the entry point to the Geode rendering backend. In **headless
 * mode** (`CreateHeadless`), it creates a WebGPU instance, selects a default
 * adapter, and creates a device - all without any window system integration.
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
  static std::unique_ptr<GeodeDevice> CreateHeadless(
      wgpu::TextureFormat textureFormat = wgpu::TextureFormat::RGBA8Unorm);

  /**
   * Create a GeodeDevice wrapping a host-provided device and queue.
   *
   * The returned device does NOT own the underlying WebGPU instance, adapter,
   * device, or queue - the host is responsible for keeping them alive.
   *
   * @param config Embedding configuration with valid device/queue handles.
   * @return A valid GeodeDevice on success, or null if \p config.device or
   *   \p config.queue is null.
   */
  static std::unique_ptr<GeodeDevice> CreateFromExternal(const GeodeEmbedConfig& config);

  /// Number of \ref CreateHeadless calls made so far in this process. Each
  /// headless creation stands up a full WebGPU instance/adapter/device, so
  /// hot paths must share one device instead of re-creating; tests pin that
  /// sharing by asserting this count stays flat across repeated operations.
  static int headlessCreationCountForTesting();

  /// Destructor releases the device and all GPU resources. All teardown
  /// waits are bounded; if the device has been declared lost (see
  /// \ref isDeviceLost) the destructor performs no GPU waits at all and
  /// deliberately leaks the root WebGPU handles (queue, device, adapter,
  /// instance) rather than risking a blocking call into a hung driver. The
  /// leak is bounded to one device's worth of driver objects per loss, and a
  /// lost device is a process-fatal condition for GPU rendering anyway.
  ~GeodeDevice();

  // Non-copyable, non-movable. The device owns pipelines and a filter
  // engine that hold a `GeodeDevice&` internally (see `filterEngine()`),
  // so moving the outer `GeodeDevice` would leave dangling references.
  // All call sites hold `GeodeDevice` via `unique_ptr` / `shared_ptr`
  // already, so deleting moves is a no-op in practice but rules out a
  // latent bug where shared-pipeline ownership is moved out from under
  // the filter engine.
  GeodeDevice(const GeodeDevice&) = delete;
  GeodeDevice& operator=(const GeodeDevice&) = delete;
  GeodeDevice(GeodeDevice&&) = delete;
  GeodeDevice& operator=(GeodeDevice&&) = delete;

  /// Returns the wgpu::Device. Guaranteed valid for the lifetime of this object.
  const wgpu::Device& device() const { return device_; }

  /// Poll the device, bracketed for ASYNCIFY suspend attribution.
  ///
  /// Under Emscripten, emdawnwebgpu implements `poll` by yielding the
  /// Asyncify-enabled thread for roughly one browser task regardless of
  /// @p wait, so every poll unwinds and later rewinds the wasm stack. With the
  /// whole application on one thread (single-canvas presenter architecture) that wall time is UI
  /// frame time, so it has to be attributable. Route every poll through here rather than calling
  /// `device().poll` directly; the probe is a pair of clock reads on native builds, where `poll`
  /// does not suspend at all.
  ///
  /// Prefer @p wait = false: a waiting poll can block inside a hung driver
  /// with no bound. Callers that need to wait for the queue to drain should
  /// use \ref waitForQueueIdle, which is bounded and reports a hang as a
  /// device-lost condition.
  ///
  /// @return True when the device reports its queue empty (wgpu-native's
  ///   `wgpuDevicePoll` return value; unspecified under Emscripten, where the
  ///   poll is a browser-task yield).
  bool pollSuspending(bool wait) const;

  /**
   * Wait, bounded, for all submitted GPU work to complete.
   *
   * Replaces unbounded `poll(wait=true)` loops: the wait is a non-blocking
   * poll at \ref kGpuWaitPollInterval cadence with a deadline, so a hung
   * driver costs at most @p timeout instead of blocking the calling thread
   * forever (in the worst case in uninterruptible kernel sleep). On timeout
   * the device is marked lost (see \ref markDeviceLost) and later waits on
   * this device return immediately.
   *
   * Under Emscripten this performs the pre-existing single poll-yield
   * instead of a bounded drain loop: emdawnwebgpu's poll return value does
   * not report queue-idle, and browser device hangs surface through the
   * readback map deadline and the browser's own device-loss reporting.
   *
   * @param timeout Wait budget; defaults to the shared generous bound.
   * @return `Complete` when the queue drained, `TimedOut` when the deadline
   *   expired (the device is now marked lost), `DeviceLost` when the device
   *   was already lost and no wait was performed.
   */
  GpuWaitResult waitForQueueIdle(std::chrono::milliseconds timeout = kDefaultGpuWaitTimeout) const;

  /// True once this device has been declared lost, either by the WebGPU
  /// device-lost callback (driver-reported) or by a bounded GPU wait
  /// exceeding its deadline. Sticky: never resets. Once lost, rendering
  /// output is undefined, snapshots return empty bitmaps promptly, and
  /// teardown skips all GPU waits.
  bool isDeviceLost() const { return lostState_->lost.load(std::memory_order_acquire); }

  /// Declare this device lost. Idempotent; the first call logs @p reason.
  /// Called from bounded waits on timeout, and available to embedders whose
  /// own device-lost signal is not shared via `GeodeEmbedConfig::lostState`.
  /// Const because observers treat the flag as shared diagnostic state and
  /// waits that discover a hang run through const accessors.
  void markDeviceLost(const char* reason) const;

  /// Instance that created the headless device. Null for externally-owned devices.
  const wgpu::Instance& instance() const;

  /// Returns the default queue.
  const wgpu::Queue& queue() const { return queue_; }

  struct ReadbackStats {
    int count = 0;
    int pollIterations = 0;
    bool usedTimedWaitAny = false;
  };

  /// Record one completed CPU readback from a renderer sharing this device.
  void recordReadback(bool usedTimedWaitAny, int pollIterations);

  /// Consume aggregate readback diagnostics for all renderers sharing this device.
  [[nodiscard]] ReadbackStats consumeReadbackStats();

  /// Returns the adapter backing this device. May be null when the host does
  /// not provide one, including embedded mode and browser headless imports.
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

  /// Number of textures waiting for the next frame-boundary destroy pass.
  /// Exposed to pin resource-retirement behavior in renderer regression tests.
  [[nodiscard]] std::size_t deferredTextureDestroyCountForTesting() const {
    return pendingTextures_.size();
  }

  /**
   * Process-unique identity for this device instance, assigned at
   * construction from a monotonic counter (never reused, starts at 1).
   *
   * Used by GPU-residence slots to detect when a
   * cached buffer / bind group belongs to a DIFFERENT device than the one
   * now rendering: a document (and its ECS `GeodeResidentPathComponent`s)
   * can outlive the device that filled them and later be rendered by a
   * second `RendererGeode` / `GeodeDevice`. WebGPU rejects cross-device
   * resources inside a render pass, so a slot whose stored id does not match
   * `deviceId()` is treated as non-resident and re-uploaded. A monotonic
   * counter (rather than a raw `this` pointer) avoids the ABA hazard of a
   * freed device's address being recycled by a later allocation.
   */
  uint64_t deviceId() const { return deviceId_; }

  /**
   * Install a `GeodeCounters` struct for this device. Non-owning; the
   * caller must keep the struct alive for as long as the device might
   * increment it. Pass `nullptr` to disable instrumentation.
   *
   * All Geode components that hold a `GeodeDevice&` route their per-frame
   * hot-path allocation and submission sites through this hook.
   */
  void setCounters(GeodeCounters* counters) { counters_ = counters; }

  /// Non-owning pointer to the installed counters, or null.
  GeodeCounters* counters() const { return counters_; }

  // Counter increment helpers. Cheap no-op when counters are disabled.
  // Also bump process-lifetime totals (`lifetimeBufferCreates_` /
  // `lifetimeTextureCreates_`) that are visible regardless of which
  // per-frame `GeodeCounters` instance is wired up - these exist so
  // issue #575's leak hunt can measure unbounded growth across whole
  // test-suite runs where each `RendererGeode` has its own scoped
  // per-frame counters.
  void countBuffer() const {
    ++lifetimeBufferCreates_;
    if (counters_) ++counters_->bufferCreates;
  }
  void countBindGroup() const {
    if (counters_) ++counters_->bindgroupCreates;
  }
  void countTexture() const {
    ++lifetimeTextureCreates_;
    if (counters_) ++counters_->textureCreates;
  }

  /// Cumulative number of `countTexture()` calls since this `GeodeDevice`
  /// was created. Does not account for textures released back into a
  /// pool - it is an allocation-site counter, not a live-count.
  uint64_t lifetimeTextureCreates() const { return lifetimeTextureCreates_; }
  /// Cumulative number of `countBuffer()` calls since this `GeodeDevice`
  /// was created. Same caveat as `lifetimeTextureCreates()`.
  uint64_t lifetimeBufferCreates() const { return lifetimeBufferCreates_; }
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
  /// Record one `wgpu::Queue::writeBuffer` call of `bytes` payload bytes.
  void countBufferWrite(uint64_t bytes) const {
    if (counters_) {
      ++counters_->bufferWrites;
      counters_->bufferWriteBytes += bytes;
    }
  }
  /// Record one `wgpu::Queue::writeTexture` call of `bytes` payload bytes.
  void countTextureWrite(uint64_t bytes) const {
    if (counters_) counters_->textureWriteBytes += bytes;
  }

  /// Shared live-resident-bytes gauge for GPU residence. Co-owned with
  /// each `GeodeResidentSlot`'s buffer so
  /// resident-memory accounting stays lifetime-safe even if a document
  /// (and its ECS registry) outlives this device. Lazily created on first
  /// access. `GeoEncoder` bumps it when a slot gains residence; the slot
  /// decrements it on reset / destruction (geometry change or document
  /// teardown), which is the eviction signal for "many distinct
  /// documents".
  const std::shared_ptr<std::atomic<int64_t>>& residentBytesGauge() const {
    if (!residentBytesGauge_) {
      residentBytesGauge_ = std::make_shared<std::atomic<int64_t>>(0);
    }
    return residentBytesGauge_;
  }

  /// Current live resident-geometry bytes across every `GeodeResidentSlot`
  /// buffer created against this device. Zero at construction and after
  /// all resident documents are torn down; used by eviction tests.
  int64_t liveResidentBytesForTesting() const {
    return residentBytesGauge_ ? residentBytesGauge_->load(std::memory_order_relaxed) : 0;
  }

  /**
   * Whether the driver supports GPU timestamp queries. Always false
   * today - reserved for future work.
   */
  bool supportsTimestamps() const { return false; }

  /// True when the active wgpu backend is Vulkan (Intel Arc hardware or Mesa
  /// lavapipe software). GeodeFilterEngine uses this to force inter-pass
  /// serialization that eliminates a nondeterministic cross-submit
  /// storage-write -> sampled-read visibility race observed on Arc Vulkan.
  /// Metal returns false and keeps the fast multi-submit path.
  bool isVulkan() const { return isVulkan_; }

  /// @name Shared dummy resources
  /// @{
  ///
  /// GeoEncoder's bind groups always include pattern + clip-mask
  /// texture/sampler slots, even when the current draw doesn't
  /// actually use them. Each slot is filled with a 1×1 "identity"
  /// texture when the feature is inactive. Previously every
  /// GeoEncoder instance created its own dummies (two textures per
  /// encoder), which showed up as 2+ `textureCreates` per frame per
  /// push/pop. Caching the dummies on the device - one instance per
  /// GeodeDevice - drops that to zero steady-state.

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
  /// clip-mask slot when no clip mask is active - the shader
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
  /// is in play.
  ///
  /// Layout mirrors the WGSL `InstanceTransform` struct in
  /// `shaders/slug_fill.wgsl`: two `vec4f` per entry, row-major affine,
  /// so the identity is `{(1, 0, 0, 0), (0, 1, 0, 0)}`. The `.z`
  /// components carry the translation (0 for identity).
  const wgpu::Buffer& identityInstanceTransformBuffer() const;
  /// @}

  /// @name Shared render / compute pipelines (issue #575 fix)
  /// @{
  ///
  /// Every wgpu pipeline created by `createRenderPipeline` /
  /// `createComputePipeline` is retained internally by wgpu-native even
  /// after the public handle's refcount drops to zero - `wgpuDevicePoll`
  /// does not drain it. Prior to this, `RendererGeode` constructed the
  /// four pipeline objects below (~18 wgpu pipelines in total, most
  /// inside `GeodeFilterEngine`) per-instance, so the image-comparison
  /// suite leaked ~1.6 MB per test and ultimately exhausted the driver
  /// memory budget (see issue #575). Moving ownership here - one copy
  /// per `GeodeDevice` - caps the pipeline footprint at a fixed cost.
  ///
  /// Every renderer that talks to this device shares the same pipeline
  /// objects; their state is intentionally immutable after construction
  /// (no per-draw mutation), so concurrent use from sibling renderers
  /// is safe as long as it is serialized at the `wgpu::Queue` level
  /// (which Donner's render path already is).

  /// Slug solid-fill render pipeline.
  GeodePipeline& pipeline() const;
  /// Slug gradient-fill render pipeline.
  GeodeGradientPipeline& gradientPipeline() const;
  /// Image-blit render pipeline (used by `GeoEncoder::drawImage` and the
  /// pattern / layer composition path).
  GeodeImagePipeline& imagePipeline() const;
  /// Clip-path mask render pipeline. Built lazily on first access rather
  /// than eagerly at device creation, matching the prior per-encoder
  /// lazy path - most documents don't use `<clipPath>` and the
  /// production WASM path avoids the cost.
  GeodeMaskPipeline& maskPipeline() const;
  /// GPU filter-graph executor. Owns ~15 compute pipelines for SVG
  /// filter primitives.
  GeodeFilterEngine& filterEngine() const;
  /// Snapshot-unpremultiply compute pipeline. Built lazily (thread-safe,
  /// once-only) on first access so consumers that never read back a snapshot
  /// avoid the compile cost.
  GeodeSnapshotReadbackPipeline& snapshotReadbackPipeline() const;

  /**
   * Acquire the pooled readback resource set for a GPU snapshot readback at
   * the given size, allocating it on first use. Repeated snapshots at the
   * same dimensions reuse the pooled entry, so steady-state snapshot readback
   * allocates nothing.
   *
   * The caller owns the returned set until `releaseSnapshotReadbackResources`
   * returns it to the pool, or until the set is destroyed unpooled. An empty
   * set means allocation failed.
   */
  SnapshotReadbackResources acquireSnapshotReadbackResources(uint32_t width, uint32_t height);

  /**
   * Return a readback resource set acquired from
   * `acquireSnapshotReadbackResources` to the device pool for reuse. The
   * returned set must be unmapped. Do not call this after the readback map
   * was cancelled and the buffer destroyed; drop the set instead.
   *
   * The pool holds at most a small fixed number of size buckets; when a
   * release would exceed that, the least-recently-used entry's backing
   * resources are destroyed (pooled entries are idle, so eager destroy is
   * safe).
   */
  void releaseSnapshotReadbackResources(SnapshotReadbackResources resources);
  /// Framebuffer checkerboard underlay pipeline used by the editor's direct
  /// presentation path. Built lazily on first access - only the editor draws
  /// it, so headless/WASM consumers never pay the compile cost.
  GeodeCheckerboardPipeline& checkerboardPipeline() const;
  /// Destination-over variant of \ref checkerboardPipeline, for targets that
  /// already hold composed premultiplied content and need the checkerboard
  /// placed underneath it. Built lazily and independently of the replace-blend
  /// pipeline, so a consumer only pays for the variant it actually draws.
  GeodeCheckerboardPipeline& checkerboardUnderlayPipeline() const;
  /// @}

private:
  GeodeDevice();

  /// Allocate the shared pipelines and filter engine after `device_`,
  /// `queue_`, and `textureFormat_` are finalised. Called from both
  /// `CreateHeadless` and `CreateFromExternal`.
  void initSharedResources();
  void initSharedPipelines();

  // Order matters: queue/device/adapter/instance destroy bottom-up.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Exposed shortcuts to Impl members (for fast access without indirection).
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::TextureFormat textureFormat_ = wgpu::TextureFormat::RGBA8Unorm;

  bool supportsTimestamps_ = false;

  /// True when the active wgpu backend is Vulkan. Set during adapter
  /// selection in CreateHeadless(); see isVulkan().
  bool isVulkan_ = false;

  /// True when this device was created via CreateFromExternal(). The destructor
  /// skips releasing the instance/adapter since the host owns them.
  bool external_ = false;

  /// Process-unique identity assigned at construction. See `deviceId()`.
  const uint64_t deviceId_ = 0;

  /// Shared device-lost flag; never null. Created at construction, replaced
  /// by the host's shared state in `CreateFromExternal` when
  /// `GeodeEmbedConfig::lostState` is provided, and retained by the WebGPU
  /// device-lost callback in headless mode (callbacks can outlive this
  /// object, hence shared ownership).
  std::shared_ptr<GeodeDeviceLostState> lostState_;

  GeodeCounters* counters_ = nullptr;

  // Process-lifetime cumulative totals - see `lifetimeTextureCreates()`
  // for why these are separate from the scoped `counters_`. Mutable
  // because `countTexture()` / `countBuffer()` are logically const
  // (the caller is reporting, not mutating visible state).
  mutable uint64_t lifetimeTextureCreates_ = 0;
  mutable uint64_t lifetimeBufferCreates_ = 0;

  std::atomic<int> readbackCount_{0};
  std::atomic<int> readbackPollIterations_{0};
  std::atomic<bool> readbackUsedTimedWaitAny_{false};

  // Shared live-resident-bytes gauge. Mutable +
  // lazily created so `residentBytesGauge()` stays const like the other
  // reporting accessors.
  mutable std::shared_ptr<std::atomic<int64_t>> residentBytesGauge_;

  // Deferred-destroy queues: resources enqueued via deferDestroy() are held
  // alive until drainDeferredDestroys() drops them at the next frame boundary.
  std::vector<ScopedWgpuHandle<wgpu::Buffer>> pendingBuffers_;
  std::vector<ScopedWgpuHandle<wgpu::Texture>> pendingTextures_;
};

}  // namespace donner::geode
