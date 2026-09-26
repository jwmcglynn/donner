#pragma once
/// @file
/// Logical rendering context over a selected native, browser, or test-reference GPU device.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeGpuContext.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"

namespace donner::svg {
class RendererGeodeTextureSnapshot;
}

namespace donner::geode {

/// 256-align a tightly packed row-byte count per the GPU runtime's
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
class GeodeGpuRoot;
struct GeodeRuntimeDevice;
class GeodeWgpuAdapterDevice;
class GeodeSnapshotReadbackPipeline;

/**
 * Shared lifetime owner for one selected GPU runtime device and the backend root it drives.
 *
 * Logical \ref GeodeDevice contexts retain this owner while keeping their runtime handle tables,
 * submissions, pipelines, caches, counters and retirement queues independent. The native root is
 * released once, after the last context.
 */
class GeodePhysicalDeviceOwner {
public:
  ~GeodePhysicalDeviceOwner();

  GeodePhysicalDeviceOwner(const GeodePhysicalDeviceOwner&) = delete;
  GeodePhysicalDeviceOwner& operator=(const GeodePhysicalDeviceOwner&) = delete;

  /// The backend root every runtime device over this owner drives.
  const GeodeGpuRoot& root() const UTILS_LIFETIME_BOUND { return *root_; }

  /// Whether this owner has a backend device that requires queue-idle handling.
  [[nodiscard]] bool hasBackendDevice() const;

  /// Sticky loss condition shared by every context and runtime device over this root. Retained
  /// because a backend device-lost callback can outlive everything that registered it.
  const std::shared_ptr<GeodeDeviceLostState>& lostState() const UTILS_LIFETIME_BOUND;

  /// A runtime device of its own over this owner's backend root, for a second logical context.
  /// Two contexts are two runtime devices: separate handle tables and serials over the one root
  /// they share.
  GeodeRuntimeDevice createLogicalDevice() const;

  /// Retirement for the context that renders through this owner's root device (see
  /// \ref GeodeDevice::handleRetirement). The owner keeps it because the root device outlives that
  /// context.
  const std::shared_ptr<GeodeHandleRetirement>& rootDeviceHandleRetirement() const
      UTILS_LIFETIME_BOUND {
    return rootDeviceRetirement_;
  }

  /// Whether the root context's retirement is destroyed after the root device: true when
  /// \c rootDeviceRetirement_ is declared before \c rootDevice_. Test accessor.
  [[nodiscard]] bool rootRetirementOutlivesRootDeviceForTesting() const;

private:
  /// Only a context builds an owner, from a root and the device \ref CreateGpuDeviceOver opened
  /// over it, so a device can never be paired with a root of another backend.
  friend class GeodeDevice;

  /**
   * Retains the selected runtime device and the backend root it drives.
   *
   * @param root Backend root the selection produced or adopted; must not be null.
   * @param device Runtime device \ref CreateGpuDeviceOver opened over \p root.
   * @return The owner, or null when either is missing.
   */
  static std::shared_ptr<GeodePhysicalDeviceOwner> Create(std::shared_ptr<GeodeGpuRoot> root,
                                                          GeodeRuntimeDevice device);

  GeodePhysicalDeviceOwner(std::shared_ptr<GeodeGpuRoot> root, std::unique_ptr<gpu::Device> device);

  /// Declared first so the backend root outlives every runtime device built over it.
  std::shared_ptr<GeodeGpuRoot> root_;
  /// Retirement of the context that renders through \ref rootDevice_. Held here and declared before
  /// it so that handles retired after that context closed go only once the device is gone
  /// (checked by \ref rootRetirementOutlivesRootDeviceForTesting).
  std::shared_ptr<GeodeHandleRetirement> rootDeviceRetirement_;
  /// The selected runtime device, held for its lifetime rather than read through here: the
  /// logical context created together with this owner is what renders through it, and every
  /// later context over the same root gets its own from \ref createLogicalDevice.
  std::unique_ptr<gpu::Device> rootDevice_;
};

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
  gpu::Texture staging;
  /// View of `staging`, kept with the pooled entry so it is created once.
  gpu::TextureView stagingView;
  /// Map-readable buffer the staging texture is copied into.
  gpu::Buffer readback;
  /// Pool key: staging texture width in pixels.
  uint32_t width = 0;
  /// Pool key: staging texture height in pixels.
  uint32_t height = 0;

  /// True when any required handle is missing; an empty set cannot be used.
  [[nodiscard]] bool empty() const {
    return !staging.isValid() || !stagingView.isValid() || !readback.isValid();
  }
};

/**
 * Registers a texture another context exported as a texture of \p consumer, and waits until what
 * \p consumer records against it next may be submitted.
 *
 * For a context that draws the registration in a frame rather than capturing it. It returns at
 * once on a backend whose contexts share one queue, and between contexts over one root on a
 * backend that orders the consumer's work after the producer's on the device (Metal), where it
 * waits only for the producer to have handed that work to its queue, which it has by the time it
 * exports. Otherwise it blocks this thread for the producer's frame.
 *
 * Loss policy. The runtime's source wait declares nothing when its budget runs out; this helper
 * is the consumer's own bounded wait and applies the policy of every other bounded wait over a
 * Geode root. A wait that spent all of \p bound means the producer's queue stopped answering: the
 * consumer's condition is declared lost with the queue-idle wait site and the measured wait, so
 * later frames fail at once instead of stalling again. A wait that ended sooner did so because a
 * device is lost or the producer failed, which is not this consumer's to report: the helper fails
 * with `DeviceLost` and declares nothing. A producer already lost is refused at registration the
 * same way.
 *
 * @param consumer Runtime device of the context that will name the texture.
 * @param source Export of the producer's texture.
 * @param bound Longest to wait for the producer's work; must be positive.
 * @return The registration, or why it was refused or could not be ordered.
 */
gpu::Result<gpu::Texture> RegisterOrderedTexture(
    gpu::Device& consumer, const gpu::TextureExport& source,
    std::chrono::milliseconds bound = kDefaultGpuWaitTimeout);

/**
 * Owns a logical Geode context over a selected GPU runtime device.
 *
 * GeodeDevice is the entry point to the Geode rendering backend. In headless mode it selects
 * the platform's native GPU root and creates a device without window system integration.
 *
 * A host that selects a presentation root creates its first context with
 * CreateOverSelectedRoot, then creates sibling contexts with CreateOverPhysicalDeviceOwner.
 *
 * Typical headless usage:
 *
 *     auto maybeDevice = GeodeDevice::CreateHeadless();
 *     if (!maybeDevice) {
 *       // No GPU available.
 *       return;
 *     }
 *
 */
class GeodeDevice {
public:
  /**
   * Create a headless GeodeDevice.
   *
   * @return A valid GeodeDevice on success, or an empty unique_ptr if the
   *   runtime could not create a GPU device (e.g., no GPU, no driver).
   */
  static std::unique_ptr<GeodeDevice> CreateHeadless(
      gpu::TextureFormat textureFormat = gpu::TextureFormat::RGBA8Unorm);

  /**
   * Creates the first logical context over a backend root the caller already selected.
   *
   * The editor and native embed example select against their actual window surface before opening
   * a logical device. This context takes the selected root's first runtime device.
   *
   * @param root Selected backend root; must not be null.
   * @param textureFormat Format the context's render targets and pipelines are built for.
   * @return A valid context, or null when the root could not be retained.
   */
  static std::unique_ptr<GeodeDevice> CreateOverSelectedRoot(std::shared_ptr<GeodeGpuRoot> root,
                                                             gpu::TextureFormat textureFormat);

  /**
   * Creates another logical context over the selected physical owner.
   *
   * It shares the backend root and loss condition with the first context while retaining its own
   * runtime device, handle table, submission serials, counters, and pipelines. Native and browser
   * roots use the same shared-owner path.
   *
   * @param physicalDevice Owner retained by the first context; null or lost owners are refused.
   * @param textureFormat Format this context's render targets and pipelines are built for.
   * @return A distinct logical context, or null when a usable device cannot be created.
   */
  static std::unique_ptr<GeodeDevice> CreateOverPhysicalDeviceOwner(
      std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice, gpu::TextureFormat textureFormat);

  /// Number of \ref CreateHeadless calls made so far in this process. Each
  /// headless creation stands up a full physical GPU root, so
  /// hot paths must share one device instead of re-creating; tests pin that
  /// sharing by asserting this count stays flat across repeated operations.
  static int headlessCreationCountForTesting();

  /// Number of retained transitional-reference loss callbacks; zero for native devices.
  static std::size_t outstandingDeviceLostCallbacksForTesting();

  /// Destructor releases logical resources before their runtime device and selected root. Teardown
  /// waits are bounded; a declared loss skips further waits into the failed device.
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

  /// Maximum supported width or height of a 2D texture on this device.
  [[nodiscard]] uint32_t maxTextureDimension2D() const;

  /**
   * Wait, bounded, for all submitted GPU work to complete.
   *
   * Native backends wait for this context's last submitted serial, rather than asking whether a
   * shared queue is momentarily empty. The explicit Linux test reference uses a bounded adapter
   * poll; the browser's imported-device path performs one yielding poll. A timeout declares the
   * device lost, and later waits return immediately.
   *
   * @param timeout Wait budget; defaults to the shared generous bound.
   * @return `Complete` when the queue drained, `TimedOut` when the deadline
   *   expired (the device is now marked lost), `DeviceLost` when the device
   *   was already lost and no wait was performed, or, on a native backend,
   *   when a loss was declared while the wait was running.
   */
  GpuWaitResult waitForQueueIdle(std::chrono::milliseconds timeout = kDefaultGpuWaitTimeout) const;

  /// Overrides one queue-idle result while preserving its loss side effects. Test seam.
  void setQueueWaitResultForTesting(std::optional<GpuWaitResult> result) {
    queueWaitResultForTesting_ = result;
  }

  /// True once this device has been declared lost, either by its backend or by a bounded GPU wait
  /// exceeding its deadline. Sticky: never resets. Once lost, rendering
  /// output is undefined, snapshots return empty bitmaps promptly, and
  /// teardown skips all GPU waits.
  bool isDeviceLost() const {
    return physicalDevice_->lostState()->lost.load(std::memory_order_acquire);
  }

  /// Whether this device may only be used on the thread that created it: true on the browser
  /// backend, whose objects belong to the worker that made them, and false on every backend whose
  /// devices any thread may drive. A cache of idle devices hands a bound device back only to the
  /// thread that created it.
  bool isBoundToCreatingThread() const;

  /// Declare this device lost. Idempotent; the first call logs @p reason.
  /// Called from bounded waits on timeout, and available to embedders whose
  /// own device-lost signal is not shared through the selected root.
  /// Const because observers treat the flag as shared diagnostic state and
  /// waits that discover a hang run through const accessors.
  void markDeviceLost(const char* reason) const;

  /**
   * Declare this device lost because a bounded wait exceeded its deadline,
   * recording which wait it was and how long it actually ran.
   *
   * Prefer this over \ref markDeviceLost at every deadline: the attribution is
   * what turns "rendering stopped" into a diagnosable report, and it is only
   * available at the wait site. Loss stays sticky, and only the call that
   * declares it records an attribution; see
   * \ref DeclareDeviceLostAfterWaitTimeout for why that rule is what keeps a
   * driver-reported loss from being relabelled as a wait timeout.
   *
   * @param site Which bounded wait expired.
   * @param elapsed Wall time that wait spent before giving up.
   * @param reason Human-readable cause, logged once like \ref markDeviceLost.
   */
  void markDeviceLostAfterWaitTimeout(GpuWaitSite site, std::chrono::milliseconds elapsed,
                                      const char* reason) const;

  struct ReadbackStats {
    int count = 0;
    int pollIterations = 0;
    bool usedTimedWaitAny = false;
    /// True once this device has been declared lost. Sticky, so every later
    /// consume keeps reporting it: a frame that never rendered has no other
    /// evidence to carry.
    bool deviceLost = false;
    /// Bounded wait that declared that loss, or `None`.
    GpuWaitSite timedOutWaitSite = GpuWaitSite::None;
    /// Wall time that wait spent before giving up, in milliseconds.
    int timedOutWaitMs = 0;
    /// Captures cancelled while acquiring the context, mapping, or copying pixels.
    uint64_t captureCancellations = 0;
    /// Captures whose total deadline expired.
    uint64_t captureTimeouts = 0;
    /// Lazily created readback-only contexts; independent of rendering pipelines.
    uint64_t contextCreates = 0;
    /// GPU allocation and submission work performed by isolated captures.
    uint64_t bufferCreates = 0;
    uint64_t textureCreates = 0;
    uint64_t bindgroupCreates = 0;
    uint64_t submits = 0;
    /// Idle pooled resource sets and their logical backing bytes after the latest capture.
    uint64_t poolEntries = 0;
    uint64_t poolBytes = 0;
    /// Bytes of this context's textures still resident because another context registered them
    /// or an export of them is alive, after this context released its own handle. No context
    /// counts that memory as its allocation, so a working set has to add it.
    uint64_t sharedTextureTailBytes = 0;
  };

  /// Override the total capture budget for deterministic cancellation/deadline tests.
  /// @param budget Includes context acquisition and GPU mapping.
  void setSnapshotReadbackBudgetForTesting(std::chrono::milliseconds budget) {
    snapshotReadbackBudgetMs_.store(
        std::clamp<int64_t>(budget.count(), 0, kReadbackMapTimeout.count()),
        std::memory_order_relaxed);
  }

  /// Explicit capture phases exposed only for deterministic protocol tests.
  enum class SnapshotReadbackPhase { WaitingForContext, ContextAcquired, MapRequested };

  /// Install a capture-phase test hook; it may be called concurrently by capture waiters.
  /// @param hook Hook to install; empty removes it. Set only while captures are quiescent.
  void setSnapshotReadbackHookForTesting(std::function<void(SnapshotReadbackPhase)> hook);

  /// Record one completed CPU readback from a renderer sharing this device.
  void recordReadback(bool usedTimedWaitAny, int pollIterations);

  /// Consume aggregate readback diagnostics for all renderers sharing this device.
  [[nodiscard]] ReadbackStats consumeReadbackStats();

  /// The sticky loss condition of the physical root this context renders through, shared with
  /// every other context over that root and with the runtime devices driving it. Retained rather
  /// than borrowed because a backend device-lost callback can outlive the context that registered
  /// it.
  const std::shared_ptr<GeodeDeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return physicalDevice_->lostState();
  }

  /// Opaque lifetime token shared by logical contexts over the same native physical root.
  std::shared_ptr<GeodePhysicalDeviceOwner> physicalDeviceOwner() const { return physicalDevice_; }

  /// Render-target texture format, chosen by the caller or defaulted to RGBA8Unorm.
  gpu::TextureFormat textureFormat() const { return textureFormat_; }

  /**
   * Enqueue a bind group for deferred destruction. The handle is kept alive until
   * `drainDeferredDestroys()` is called: a bind group evicted from a cache may still be named by
   * draws that have been recorded but not yet replayed to the backend, and destroying it there
   * fails those draws closed.
   */
  void deferDestroy(gpu::BindGroup bindGroup);

  /**
   * Transfer owned texture backing to this context's thread-safe retirement mailbox.
   *
   * Enqueuing checks only immutable device identity and never touches the resource table.
   * The owning rendering context or exclusive teardown must drain the mailbox.
   *
   * @param texture Handle consumed on success; unchanged for a null or foreign handle.
   * @return Whether the handle was accepted.
   */
  [[nodiscard]] bool deferDestroyTextureBacking(gpu::Texture&& texture);

  /// Destroy queued texture backing on the owning rendering context or during exclusive teardown.
  void drainDeferredTextureBackings();

  /**
   * Where state kept for this context on another thread's behalf - a document's resident slabs,
   * say - hands back this context's buffers and bind groups when it is destroyed, so they are
   * released on this context's thread (see \ref GeodeHandleRetirement). Shared so that state can
   * tell whether this context still exists: it is closed when the context is destroyed.
   */
  const std::shared_ptr<GeodeHandleRetirement>& handleRetirement() const UTILS_LIFETIME_BOUND {
    return handleRetirement_;
  }

  /// Releases the handles other threads retired to this context. Call on this context's thread;
  /// the renderer does at every frame boundary.
  void releaseRetiredHandles();

  /// Handles retired to this context and not yet released. Test accessor.
  [[nodiscard]] GeodeHandleRetirement::HeldCounts retiredHandleCountsForTesting() const;

  /// Whether this context's retirement is destroyed after the runtime device it created, as
  /// \ref handleRetirement requires: true when \c handleRetirement_ is declared before
  /// \c ownedRuntimeDevice_. Test accessor.
  [[nodiscard]] bool retirementOutlivesOwnedRuntimeDeviceForTesting() const;

  /**
   * Drop all deferred-destroy handles, releasing their GPU resources.
   *
   * Called at the top of each frame (before new allocations) so resources
   * from the previous frame's command buffer submission have had time to
   * complete on the GPU. The runtime retires backing resources after submitted work completes.
   */
  void drainDeferredDestroys();

  /// Nonblocking owner-thread maintenance after a frame or while the renderer is idle. Drains
  /// cross-thread mailboxes, pumps callback-driven backends once, and releases completed work.
  /// No new submission or queue wait is required.
  void pollIdle();

  /// Whether idle maintenance can still free resources when a completion arrives. Owner-thread
  /// only; event loops may use it to schedule a short maintenance wake instead of rendering.
  [[nodiscard]] bool hasIdleWork() const;

  /// Posts a cheap wake when a cross-thread retirement arrives. Callbacks must not reenter this
  /// context and must be cleared before their event loop is destroyed.
  void setIdleWakeCallback(std::function<void()> callback);

  /// Number of texture backings waiting for the next frame-boundary destroy pass.
  /// Exposed to pin resource-retirement behavior in renderer regression tests.
  [[nodiscard]] std::size_t deferredTextureDestroyCountForTesting() const;

  /**
   * Waits until the snapshot capture context's queue has no submitted work left, so a test can
   * check what the owner's next release point frees once every capture has finished on the GPU,
   * without polling for it. Waits for a capture in progress to end first.
   *
   * @param timeout Bound on the queue wait; a timeout is declared like any queue-idle wait.
   * @return \ref GpuWaitResult::Complete when the queue is idle or no capture context exists.
   */
  [[nodiscard]] GpuWaitResult waitForSnapshotCaptureIdleForTesting(
      std::chrono::milliseconds timeout = kDefaultGpuWaitTimeout);

  /**
   * Key identifying a scene-batch bind group by its exact buffer bindings:
   * the shared batch uniform allocation, the geometry slab chunk, and the
   * record span. The dummy texture/sampler bindings are device-owned
   * singletons, so they never contribute to the key.
   *
   * Scene batches bind pooled arena and slab buffers whose identities and
   * offsets are stable across steady-state frames, so a cached bind group
   * keyed this way is reusable frame over frame (keeping the per-frame
   * bind-group create count flat; the GeodePerf ceilings pin this).
   *
   * Buffers are identified by the process-unique ids handed out by
   * `AllocateBufferId()`, rather than a backend handle address that may be recycled when a
   * document releases its slab. A device outlives the documents drawn on it, so an address reused
   * by another document must never retrieve the previous document's cached bind group.
   */
  struct SceneBatchBindGroupKey {
    uint64_t uniformBufferId = 0;
    uint64_t uniformOffset = 0;
    uint64_t uniformSize = 0;
    uint64_t chunkBufferId = 0;
    uint64_t chunkBytes = 0;
    uint64_t recordBufferId = 0;
    uint64_t recordOffset = 0;
    uint64_t recordBytes = 0;

    friend bool operator==(const SceneBatchBindGroupKey& a,
                           const SceneBatchBindGroupKey& b) = default;

    friend bool operator<(const SceneBatchBindGroupKey& a, const SceneBatchBindGroupKey& b) {
      return std::tie(a.uniformBufferId, a.uniformOffset, a.uniformSize, a.chunkBufferId,
                      a.chunkBytes, a.recordBufferId, a.recordOffset, a.recordBytes) <
             std::tie(b.uniformBufferId, b.uniformOffset, b.uniformSize, b.chunkBufferId,
                      b.chunkBytes, b.recordBufferId, b.recordOffset, b.recordBytes);
    }
  };

  /**
   * Allocate a process-unique identity for a GPU buffer handle, from a
   * monotonic counter that starts at 1 and is never reused.
   *
   * Anything that outlives a buffer and still has to answer "is this the
   * same buffer I saw before?" must compare these ids rather than
   * backend handle addresses. The runtime may release a handle while submitted work still retains
   * its allocation; a later allocation can reuse an address and make unrelated buffers appear
   * identical. `deviceId()` exists for the same reason one level up.
   *
   * `0` is reserved for "no buffer", so a default-constructed id never
   * matches a real one.
   */
  [[nodiscard]] static uint64_t AllocateBufferId();

  /// Look up a cached scene-batch bind group. Returns a borrowed handle
  /// (valid while the cache entry lives), or an empty handle on miss. A hit
  /// refreshes the entry to most-recently-used, so eviction at the cap drops
  /// the coldest entry rather than the oldest-inserted one - with more than
  /// the cap of live groups, pure insertion order would evict the hottest
  /// steady-state entry every frame.
  [[nodiscard]] const gpu::BindGroup* findSceneBatchBindGroup(const SceneBatchBindGroupKey& key);

  /// Store a scene-batch bind group under `key`, taking ownership of the
  /// +1 handle. At the cap, the OLDEST entries are evicted one at a time
  /// rather than the whole cache being dropped: the cache is device-wide and
  /// a key belongs to one document's buffers, so wholesale clearing punished
  /// the live document for documents that had already been torn down, and a
  /// steady frame then rebuilt every batch group it had just been using.
  /// Insertion order puts those dead entries first. Recorded command buffers
  /// keep their own references, so dropping the cache's handle mid-frame is
  /// safe either way.
  const gpu::BindGroup& storeSceneBatchBindGroup(const SceneBatchBindGroupKey& key,
                                                 gpu::BindGroup group);

  /**
   * Process-unique identity for this device instance, assigned at
   * construction from a monotonic counter (never reused, starts at 1).
   *
   * Keys the GPU residence a document keeps per device (see
   * `GeodePerDevice`), so each device that draws a document finds its own
   * slabs and slots and never another's. Residence slots also record it, and
   * the draw path treats a slot whose id does not match as non-resident, a
   * defensive cross-check since GPU backends reject cross-device resources inside
   * a render pass. A monotonic counter (rather than a raw `this` pointer)
   * avoids the ABA hazard of a freed device's address being recycled by a
   * later allocation.
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
    if (counters_) {
      ++counters_->bufferCreates;
    }
  }
  void countBindGroup() const {
    if (counters_) {
      ++counters_->bindgroupCreates;
    }
  }
  void countTexture() const {
    ++lifetimeTextureCreates_;
    if (counters_) {
      ++counters_->textureCreates;
    }
  }

  /// Cumulative number of `countTexture()` calls since this `GeodeDevice`
  /// was created. Does not account for textures released back into a
  /// pool - it is an allocation-site counter, not a live-count.
  uint64_t lifetimeTextureCreates() const {
    return lifetimeTextureCreates_ +
           readbackLifetimeTextureCreates_.load(std::memory_order_relaxed);
  }
  /// Record that this context's runtime device gave up its ownership of a texture allocation.
  void countTextureRelease() const { ++lifetimeTextureReleases_; }
  /// Texture allocations whose ownership this context's runtime device has given up since the
  /// context was created (see `gpu::DeviceObserver::onTextureReleased`), on every backend. It
  /// excludes the snapshot-capture context, whose creations `lifetimeTextureCreates()` includes, so
  /// creates minus releases is not a live count.
  uint64_t lifetimeTextureReleases() const { return lifetimeTextureReleases_; }
  /// Cumulative number of `countBuffer()` calls since this `GeodeDevice`
  /// was created. Same caveat as `lifetimeTextureCreates()`.
  uint64_t lifetimeBufferCreates() const {
    return lifetimeBufferCreates_ + readbackLifetimeBufferCreates_.load(std::memory_order_relaxed);
  }
  void countSubmit() const {
    if (counters_) {
      ++counters_->submits;
    }
  }
  /// Record the command buffers one submission carried.
  /// @param count Command buffers handed to the queue together.
  void countCommandBuffers(uint64_t count) const {
    if (counters_) {
      counters_->commandBuffers += count;
    }
  }
  void countPathEncode() const {
    if (counters_) {
      ++counters_->pathEncodes;
    }
  }
  /// Record the draws a submission counted (see \ref gpu::DeviceObserver::onSubmitted).
  /// @param count Draws counted.
  void countDraws(uint64_t count) const {
    if (counters_ != nullptr) {
      counters_->drawCalls += count;
    }
  }
  void countPipelineSwitch() const {
    if (counters_) {
      ++counters_->pipelineSwitches;
    }
  }
  /// Record one runtime buffer write of `bytes` payload bytes.
  void countBufferWrite(uint64_t bytes) const {
    if (counters_) {
      ++counters_->bufferWrites;
      counters_->bufferWriteBytes += bytes;
    }
  }
  /// Record one runtime texture write of `bytes` payload bytes.
  void countTextureWrite(uint64_t bytes) const {
    if (counters_) {
      counters_->textureWriteBytes += bytes;
    }
  }
  /**
   * Open a frame on this device and return its generation.
   *
   * Generations are DEVICE-scoped and monotonic, so two renderers sharing one
   * device cannot mint the same index. That happens routinely: an offscreen
   * renderer built from this device renders an `feImage` fragment or a layer
   * thumbnail from the same document while the outer renderer's frame is still
   * open and has already recorded draws. With per-renderer counters the two
   * frames alias, and a cache keyed on "was this touched in the current frame?"
   * cannot tell the inner pass from the outer one - it would let the inner pass
   * rewrite buffers the outer pass's recorded draws read, and every buffer
   * write in a frame lands before every draw in that frame's submit.
   *
   * Pair with `endFrameGeneration`. A cache that must not recycle memory an
   * unsubmitted frame still reads compares against
   * `oldestOpenFrameGeneration()` rather than against its own index.
   */
  uint64_t beginFrameGeneration() {
    const uint64_t generation = ++frameGeneration_;
    openFrameGenerations_.push_back(generation);
    return generation;
  }

  /// Close a generation opened by `beginFrameGeneration`. Unknown values are
  /// ignored, so a renderer that never opened one (no-op mode) is safe.
  void endFrameGeneration(uint64_t generation) {
    const auto it =
        std::find(openFrameGenerations_.begin(), openFrameGenerations_.end(), generation);
    if (it != openFrameGenerations_.end()) {
      openFrameGenerations_.erase(it);
    }
  }

  /// Oldest generation whose frame has not closed yet, or `~0` when no frame is
  /// open. Anything a generation at or after this touched may still be read by
  /// an unsubmitted draw.
  uint64_t oldestOpenFrameGeneration() const {
    uint64_t oldest = ~uint64_t{0};
    for (const uint64_t generation : openFrameGenerations_) {
      oldest = std::min(oldest, generation);
    }
    return oldest;
  }

  /**
   * True when a resource stamped with `stamp` (a generation minted by
   * `beginFrameGeneration`) may still be read by a frame that has not
   * submitted: some open frame is at or before the stamp, so a recorded draw
   * in that frame can reference the stamped bytes, and every queue write in a
   * frame lands before every draw in its submit.
   *
   * The never-drawn sentinel `~0` used by the resident-slot stamps is
   * explicitly NOT claimed - a naive `>=` would read a never-drawn slot as
   * claimed forever, since `oldestOpenFrameGeneration()` is also `~0` when no
   * frame is open.
   */
  [[nodiscard]] bool frameStampClaimed(uint64_t stamp) const {
    return stamp != ~uint64_t{0} && stamp >= oldestOpenFrameGeneration();
  }

  /// Record one glyph occurrence served from an already-resident outline.
  void countGlyphResidencyHit() const {
    if (counters_) {
      ++counters_->glyphResidencyHits;
    }
  }
  /// Record one unique glyph outline encoded and made resident.
  void countGlyphResidencyUpload() const {
    if (counters_) {
      ++counters_->glyphResidencyUploads;
    }
  }
  /// Record `count` cached glyph outlines dropped to stay inside the budget.
  void countGlyphResidencyEvictions(uint64_t count) const {
    if (counters_) {
      counters_->glyphResidencyEvictions += count;
    }
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

  /// True when the active native backend is Vulkan (Intel Arc hardware or Mesa
  /// lavapipe software). GeodeFilterEngine uses this to force inter-pass
  /// serialization that eliminates a nondeterministic cross-submit
  /// storage-write -> sampled-read visibility race observed on Arc Vulkan.
  /// Metal returns false and keeps the fast multi-submit path.
  bool isVulkan() const;

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

  /// The shared bind-slot resources - 1x1 dummy pattern texture and view, 1x1 full-coverage
  /// clip-mask texture and view, their samplers, a one-element identity instance record, and a
  /// zero-filled gradient paint block - are reached through \ref gpuContext.
  /// @}

  /// @name Shared render / compute pipelines (issue #575 fix)
  /// @{
  ///
  /// RendererGeode once constructed these pipeline objects per renderer instance, exhausting the
  /// GPU memory budget in image-comparison runs (issue #575). Ownership here keeps one fixed set
  /// per GeodeDevice instead.
  ///
  /// Every renderer that talks to this device shares the same pipeline
  /// objects; their state is intentionally immutable after construction
  /// (no per-draw mutation), so concurrent use from sibling renderers
  /// is safe as long as submissions are serialized by the selected runtime device.

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

  /// This context's GPU runtime device: the owner of its handle tables, submission serials, and
  /// resource retirement. Renderer services that need only the runtime contract take this instead
  /// of naming the concrete backend type.
  gpu::Device& runtimeDevice() const UTILS_LIFETIME_BOUND;

#ifdef DONNER_GEODE_WGPU_REFERENCE
  /// Whether this context renders through the transitional adapter, so \ref adapterDevice names
  /// a device. Available only to the explicit Linux resvg reference configuration.
  bool hasTransitionalAdapter() const;

  /// Transitional adapter used by the Linux resvg reference, never by a native product context.
  GeodeWgpuAdapterDevice& adapterDevice() const UTILS_LIFETIME_BOUND;
#endif

  /// The recording context Geode's encoders record a frame against: this device's GPU runtime
  /// device, its shared bind-slot resources, and its counter sinks. Wired once with the shared
  /// pipelines and owned here, so it lives exactly as long as this device does.
  const GeodeGpuContext& gpuContext() const UTILS_LIFETIME_BOUND;

  /**
   * The observer this context installs on its own runtime device, on every backend. It reports
   * what that device allocates, uploads, and submits through the `count*` members above, so the
   * counters follow the same rules whichever backend renders. Texture upload bytes are what each
   * backend hands its queue, so they can differ where a backend repacks rows.
   *
   * A test that installs it on another runtime device attributes that device's work to this
   * context as well, and removes it again before either is destroyed.
   */
  gpu::DeviceObserver& runtimeCounterObserverForTesting() const UTILS_LIFETIME_BOUND;

private:
  friend class svg::RendererGeodeTextureSnapshot;

  /// Forwards a runtime device's notifications to this context's counters.
  class RuntimeCounterObserver;

  enum class SnapshotCaptureStatus { Ready, Cancelled, TimedOut };
  struct SnapshotCaptureLease {
    SnapshotCaptureLease() = default;
    SnapshotCaptureLease(SnapshotCaptureLease&&) = default;
    SnapshotCaptureLease& operator=(SnapshotCaptureLease&&) = delete;
    ~SnapshotCaptureLease();
    GeodeDevice* owner = nullptr;
    GeodeDevice* context = nullptr;
    std::unique_lock<std::timed_mutex> lock;
    SnapshotCaptureStatus status = SnapshotCaptureStatus::TimedOut;
  };

  SnapshotCaptureLease acquireSnapshotCapture(const std::function<bool()>& shouldCancel,
                                              std::chrono::steady_clock::time_point deadline);

  /**
   * Registers a texture the producer exported as a texture of this capture context.
   *
   * A capture context is a runtime device of its own, so a texture of the producer is not a
   * texture of the capture until it is named here. The export was made on the producer's thread,
   * so registering it never reads the producer's tables. The registration describes the texture
   * the way its producer does, never owns the allocation, and is forgotten when the returned
   * handle goes away; the runtime refuses it for a producer over a different backend device, for a
   * texture its producer has released, and on a backend that cannot share textures. Only a
   * capture context may register a source.
   *
   * @param source Export of the texture to capture.
   */
  gpu::Result<gpu::Texture> registerCaptureSource(const gpu::TextureExport& source);

  SnapshotCaptureStatus waitForSnapshotCapture(std::unique_lock<std::timed_mutex>& lock,
                                               const std::function<bool()>& shouldCancel,
                                               std::chrono::steady_clock::time_point deadline);
  void finishSnapshotCapture(GeodeDevice& context);

  /**
   * Recycles what the snapshot capture context has retired and the GPU has finished with, when no
   * capture holds the context.
   *
   * A capture that ends before its readback completes (cancelled, past its deadline, or failed)
   * retires its registration of the source texture with that readback in flight, and only a poll
   * of the capture context recycles it. Until then the registration holds the texture, so the
   * owner could not release it. The owner calls this before releasing textures.
   */
  void pollIdleSnapshotCaptureContext();
  void recordSnapshotCaptureTimeout();
  void notifySnapshotReadbackPhaseForTesting(SnapshotReadbackPhase phase) const;
  GeodeSnapshotReadbackPipeline& snapshotReadbackPipeline() const;
  SnapshotReadbackResources acquireSnapshotReadbackResources(uint32_t width, uint32_t height);
  void releaseSnapshotReadbackResources(SnapshotReadbackResources resources);

  /**
   * Builds a logical context over \p physicalDevice, rendering through \p runtimeDevice.
   *
   * @param physicalDevice Owner retained for the whole context lifetime.
   * @param runtimeDevice Runtime device to render through; either the owner's root device, which
   *   the context created together with the owner takes, or one of its own from
   *   \ref GeodePhysicalDeviceOwner::createLogicalDevice.
   * @param transitionalAdapter \p runtimeDevice named as the transitional adapter, as
   *   \ref CreateGpuDeviceOver recorded it; null on a native backend.
   * @param ownedRuntimeDevice Non-null when \p runtimeDevice is this context's own, so the
   *   context releases it; null when it is the owner's.
   */
  GeodeDevice(std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice, gpu::Device& runtimeDevice,
              GeodeWgpuAdapterDevice* transitionalAdapter,
              std::unique_ptr<gpu::Device> ownedRuntimeDevice);

  /// Builds a logical context with a runtime device of its own over \p physicalDevice's root.
  /// @param physicalDevice Owner whose root the new context renders through.
  /// @return The context, or null when the owner could not stand up a runtime device.
  static std::unique_ptr<GeodeDevice> CreateLogicalContext(
      std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice);

  /// Creates the shared bind-slot resources through the GPU runtime and wires \ref gpuContext.
  /// Runs with the shared pipelines, once the adapter exists.
  void initSharedBindSlotResources();

  void initSharedPipelines();

  // Declared before every logical resource so it is destroyed last.
  std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice_;

  /// See \ref handleRetirement: the owner's root-device retirement when this context renders
  /// through the root device, else one of its own. Closed at teardown while the runtime device
  /// still exists, and declared before \ref ownedRuntimeDevice_ so an own retirement, and the
  /// handles retired to it after it closed, go only once that device is gone (checked by
  /// \ref retirementOutlivesOwnedRuntimeDeviceForTesting).
  std::shared_ptr<GeodeHandleRetirement> handleRetirement_;

  /// Held only when this context created its own runtime device; null when it renders through the
  /// owner's. Declared before \ref impl_ so the pipelines and pooled resources there, which
  /// release their handles through this device, are destroyed while it still exists.
  std::unique_ptr<gpu::Device> ownedRuntimeDevice_;

  /// This context's runtime device: \ref ownedRuntimeDevice_, or the owner's root device. Never
  /// null once construction has finished, and kept out of \ref impl_ so teardown can still drain
  /// the queue after the logical resources are gone.
  gpu::Device* runtimeDevice_ = nullptr;

  /// \ref runtimeDevice_ named by its concrete type when this context renders through the
  /// transitional adapter, and null on a native backend, where the operations that accessor
  /// exists for have no wgpu object to reach.
  GeodeWgpuAdapterDevice* transitionalAdapter_ = nullptr;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  gpu::TextureFormat textureFormat_ = gpu::TextureFormat::RGBA8Unorm;

  bool readbackOnly_ = false;
  GeodeCounters isolatedReadbackCounters_;

  /// Process-unique identity assigned at construction. See `deviceId()`.
  const uint64_t deviceId_ = 0;

  /// Monotonic device-scoped frame counter; see `beginFrameGeneration()`.
  uint64_t frameGeneration_ = 0;
  /// Generations opened but not yet closed. At most a handful are ever live
  /// (an outer frame plus its nested offscreen passes), so a linear scan is
  /// cheaper than any ordered container.
  std::vector<uint64_t> openFrameGenerations_;

  GeodeCounters* counters_ = nullptr;

  /// Installed on this context's runtime device for the context's whole lifetime.
  std::unique_ptr<RuntimeCounterObserver> runtimeCounterObserver_;

  // Process-lifetime cumulative totals - see `lifetimeTextureCreates()`
  // for why these are separate from the scoped `counters_`. Mutable
  // because `countTexture()` / `countBuffer()` are logically const
  // (the caller is reporting, not mutating visible state).
  mutable uint64_t lifetimeTextureCreates_ = 0;
  mutable uint64_t lifetimeTextureReleases_ = 0;
  mutable uint64_t lifetimeBufferCreates_ = 0;

  std::atomic<int> readbackCount_{0};
  std::atomic<int> readbackPollIterations_{0};
  std::atomic<bool> readbackUsedTimedWaitAny_{false};
  std::atomic<int64_t> snapshotReadbackBudgetMs_{kReadbackMapTimeout.count()};
  std::atomic<uint64_t> readbackCaptureCancellations_{0};
  std::atomic<uint64_t> readbackCaptureTimeouts_{0};
  std::atomic<uint64_t> readbackContextCreates_{0};
  std::atomic<uint64_t> readbackBufferCreates_{0};
  std::atomic<uint64_t> readbackTextureCreates_{0};
  std::atomic<uint64_t> readbackBindgroupCreates_{0};
  std::atomic<uint64_t> readbackSubmits_{0};
  mutable std::optional<GpuWaitResult> queueWaitResultForTesting_;
  std::atomic<uint64_t> readbackPoolEntries_{0};
  std::atomic<uint64_t> readbackPoolBytes_{0};
  std::atomic<uint64_t> readbackLifetimeBufferCreates_{0};
  std::atomic<uint64_t> readbackLifetimeTextureCreates_{0};

  // Shared live-resident-bytes gauge. Mutable +
  // lazily created so `residentBytesGauge()` stays const like the other
  // reporting accessors.
  mutable std::shared_ptr<std::atomic<int64_t>> residentBytesGauge_;

  // Deferred-destroy queue: bind groups enqueued via deferDestroy() are held
  // alive until drainDeferredDestroys() drops them at the next frame boundary.
  std::vector<gpu::BindGroup> pendingBindGroups_;

  // Scene-batch bind groups cached across frames (see
  // SceneBatchBindGroupKey). Bounded by kSceneBatchBindGroupCacheCap, with
  // `sceneBatchBindGroupOrder_` recording insertion order so the eviction at
  // the cap drops the oldest entries instead of everything.
  static constexpr std::size_t kSceneBatchBindGroupCacheCap = 256;
  std::map<SceneBatchBindGroupKey, gpu::BindGroup> sceneBatchBindGroups_;
  std::deque<SceneBatchBindGroupKey> sceneBatchBindGroupOrder_;
};

}  // namespace donner::geode
