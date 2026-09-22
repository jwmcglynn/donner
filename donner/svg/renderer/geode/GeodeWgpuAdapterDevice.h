#pragma once
/// @file
/// \c donner::geode::GeodeWgpuAdapterDevice - the wgpu-backed \c donner::gpu::Device adapter.
///
/// TEMPORARY transition adapter. It is deleted per-platform as each native backend takes over
/// production rendering, and each escape hatch below is deleted with the change that migrates its
/// last caller.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/base/SmallVector.h"
#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeDevice;
class GeodeWgpuAdapterDevice;

/// The backend objects one wgpu device is reached through, and who releases them.
struct GeodeWgpuRoots {
  wgpu::Instance instance;  //!< Instance the adapter came from; null when a host supplied none.
  wgpu::Adapter adapter;    //!< Adapter the device came from; null when a host supplied none.
  wgpu::Device device;      //!< Device every runtime device over this root records against.
  wgpu::Queue queue;        //!< Default queue of \ref device.
  /// Whether releasing the handles above is Donner's job. False for an embedder's roots, which
  /// outlive every context built over them and belong to the embedder.
  bool owned = false;
  /// Token the device-lost callback this process installed retains, or null when it installed
  /// none. Released with the handles.
  void* deviceLostCallbackToken = nullptr;
};

/// Which backend implementation a selection builds its runtime devices from.
enum class GpuBackendKind : uint8_t {
  /// The transitional wgpu-native adapter, on whichever API wgpu selects underneath.
  TransitionalWgpu,
  /// The native Metal backend of the Donner GPU runtime. Apple platforms only.
  NativeMetal,
};

/// What a selection discovered about a backend root, queried once because every runtime device
/// over the root answers these identically.
struct GeodeGpuRootCapabilities {
  /// Backend the selection produced. Decides which runtime device \ref CreateGpuDeviceOver
  /// builds, and whether the wgpu handles on the root name anything.
  GpuBackendKind backend = GpuBackendKind::TransitionalWgpu;
  /// Maximum supported width or height of a 2D texture, as the selected device reports it.
  /// WebGPU guarantees at least 8,192, which is the fail-closed fallback when a device cannot
  /// report its limits.
  uint32_t maxTextureDimension2D = 8192u;
  /// Whether the active backend is Vulkan (Intel Arc hardware or Mesa lavapipe software).
  /// GeodeFilterEngine uses this to force the inter-pass serialization that eliminates a
  /// nondeterministic cross-submit storage-write to sampled-read visibility race seen only there;
  /// Metal keeps the fast multi-submit path.
  bool isVulkan = false;
};

/**
 * One selected backend root: the wgpu objects a set of runtime devices drives, the capabilities
 * the selection discovered, and the sticky loss condition every one of them shares.
 *
 * Retained through `shared_ptr` by each runtime device over it, so the handles outlive the last
 * of them. Produced only by \ref SelectGpuRoot and \ref AdoptGpuRoot: assembling roots field by
 * field is what let a half-populated set escape to a caller, and a root that exists is a root
 * that is complete.
 */
class GeodeGpuRoot {
public:
  /**
   * Retains one complete set of backend roots.
   *
   * @param handles Backend objects the selection produced or adopted.
   * @param capabilities What the selection discovered about them.
   * @param lostState Sticky loss condition shared by every runtime device over these roots.
   */
  GeodeGpuRoot(GeodeWgpuRoots handles, GeodeGpuRootCapabilities capabilities,
               std::shared_ptr<gpu::DeviceLostState> lostState);

  /// Releases owned handles, or leaves borrowed ones to their embedder. A root already declared
  /// lost is deliberately leaked rather than destroyed: releasing it calls into a driver that has
  /// stopped answering, and one root's worth of driver objects is strictly better than a hung
  /// thread.
  ~GeodeGpuRoot();

  GeodeGpuRoot(const GeodeGpuRoot&) = delete;
  GeodeGpuRoot& operator=(const GeodeGpuRoot&) = delete;

  /// Instance the adapter came from, or null. Borrowed; the root retains it.
  const wgpu::Instance& instance() const UTILS_LIFETIME_BOUND { return handles_.instance; }
  /// Adapter the device came from, or null. Borrowed; the root retains it.
  const wgpu::Adapter& adapter() const UTILS_LIFETIME_BOUND { return handles_.adapter; }
  /// Device every runtime device over this root records against. Borrowed.
  const wgpu::Device& device() const UTILS_LIFETIME_BOUND { return handles_.device; }
  /// Default queue of \ref device. Borrowed.
  const wgpu::Queue& queue() const UTILS_LIFETIME_BOUND { return handles_.queue; }
  /// What the selection discovered about this root.
  const GeodeGpuRootCapabilities& capabilities() const UTILS_LIFETIME_BOUND {
    return capabilities_;
  }
  /// Sticky loss condition shared by every runtime device over this root.
  const std::shared_ptr<gpu::DeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return lostState_;
  }

  /**
   * Whether every non-null handle named here is the one this root holds.
   *
   * An embedder that passes both a shared root and explicit handles is stating they name the same
   * objects; a mismatch means one of the two is wrong, and rendering through the wrong one is
   * undiagnosable. Null names nothing and always agrees.
   *
   * @param instance Instance to compare, or null.
   * @param adapter Adapter to compare, or null.
   * @param device Device to compare, or null.
   * @param queue Queue to compare, or null.
   */
  bool names(const wgpu::Instance& instance, const wgpu::Adapter& adapter,
             const wgpu::Device& device, const wgpu::Queue& queue) const;

  /// Whether a runtime device over this root still has a backend to record against. The
  /// transitional adapter records through the wgpu device and queue held here; a native backend
  /// is reached through the runtime device itself, so this root holds no handles for it.
  bool hasBackendDevice() const;

private:
  GeodeWgpuRoots handles_;
  GeodeGpuRootCapabilities capabilities_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
};

/// Caller-supplied inputs to backend-root selection. The environment-driven inputs (the backend
/// override and the force-fallback-adapter request) are read by the selection itself, so every
/// caller honors them without repeating them.
struct GpuRootSelection {
  /// Label the selected device carries in driver diagnostics.
  std::string_view label = "GeodeDevice";
  /// Prepares whatever the caller presents to and reports the surface adapter selection must be
  /// constrained to, called once with the created instance. Absent for callers that render
  /// offscreen.
  ///
  /// `std::nullopt` aborts the selection: the caller could not build what it meant to present to,
  /// and handing back a device that cannot present to it would fail later and further from the
  /// cause. A null surface inside the optional is a caller that presents through a platform object
  /// no surface can constrain - a Metal layer presents from any Metal adapter the system reports -
  /// so selection is left unconstrained.
  std::function<std::optional<wgpu::Surface>(const wgpu::Instance&)> compatibleSurface;

  /// Backend to select. The transitional adapter is the default on every platform; a native
  /// backend the platform does not have is refused rather than silently falling back, because a
  /// run whose expectations were recorded against one backend and which lands on another is a
  /// failure that looks like a rendering bug.
  GpuBackendKind backend = GpuBackendKind::TransitionalWgpu;

  /// Whether an absent `WGPU_BACKEND` override falls back to the platform's preferred backend
  /// rather than leaving the choice to the driver.
  ///
  /// The headless entry point does: a run whose expectations were recorded against one backend
  /// and which silently lands on another is a failure that looks like a rendering bug. The editor
  /// does not. Its window is served by whatever backend the system can drive its surface with,
  /// and narrowing that leaves a host whose preferred backend is unusable with no adapter at all;
  /// its offscreen target takes the same answer so that one editor does not select two different
  /// ways depending on where its frames go.
  bool usePlatformDefaultBackend = true;
};

/**
 * Selects a backend root: creates an instance, requests an adapter and a device, and takes the
 * default queue.
 *
 * The one selection every caller shares. Headless, editor and embedded construction differ only
 * in \p options, so the adapter retries under load, the backend override, the force-fallback
 * request, the device-lost callback and the uncaptured-error reporting are decided once rather
 * than per caller. Under Emscripten the browser's device is imported instead, which is the same
 * decision expressed the only way that platform allows.
 *
 * @param options Caller-supplied inputs; the rest come from the environment.
 * @return The selected root, or null when no adapter or device could be obtained.
 */
std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options);

/**
 * Adopts backend roots the caller created and keeps alive.
 *
 * Borrowed: Donner releases nothing here, and the caller must outlive every context built over
 * them.
 *
 * @param handles Host-provided backend objects; \c device and \c queue must be non-null.
 * @param lostState Loss condition to share with the host, or null for a private one that only
 *   Donner's own bounded waits can set.
 * @return The adopted root, or null when \p handles names no device or queue.
 */
std::shared_ptr<GeodeGpuRoot> AdoptGpuRoot(const GeodeWgpuRoots& handles,
                                           std::shared_ptr<gpu::DeviceLostState> lostState);

/**
 * Creates one runtime device over \p root.
 *
 * Every logical rendering context gets its own: two contexts over one root are two runtime
 * devices with their own handle tables, submission serials and counters, so a handle minted by
 * one cannot pass validation on the other. They share the root's loss condition, because the root
 * is what stops answering.
 *
 * @param root Root to render through; must not be null.
 */
std::unique_ptr<gpu::Device> CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root);

/**
 * The backend kind named by `DONNER_GPU_BACKEND`, or \p fallback when it names none.
 *
 * One process-wide override so a suite can be run end to end against a backend that is not yet
 * the default, without a second copy of every target. An unrecognized value is reported and
 * ignored.
 *
 * @param fallback Kind to use when the variable is absent or empty.
 */
GpuBackendKind RequestedGpuBackendKind(GpuBackendKind fallback);

/// Retained device-lost callback states this process has not yet seen the backend consume. A
/// selection that gave up mid-retry strands at most one per attempt, so teardown tests assert this
/// returns to zero.
std::size_t OutstandingDeviceLostCallbacks();

/// Backend instances this process created for a selection and has not released. A selection that
/// fails must leave this where it found it, because nothing else can release the objects it built
/// before giving up; a root released at teardown returns its own.
/// @return Instances created for a selection and not yet released.
std::size_t OutstandingSelectionInstances();

/**
 * Implements \c donner::gpu::Device on top of one selected backend root, so Geode subsystems can
 * migrate onto the Donner GPU runtime one at a time while the process still renders through wgpu
 * underneath.
 *
 * Every `on*` hook receives input the base class already validated fail-closed, and translates
 * it to the corresponding webgpu.hpp call. wgpu objects are stored in per-kind slot vectors
 * indexed by the base class's slot indices; bind groups capture their wgpu layout object at
 * creation time (wgpu retains it internally), so encoding never resolves a layout by slot.
 *
 * Retains its root, so the backend handles outlive it. It does NOT own the \ref GeodeDevice that
 * installs itself as the counter sink; that context must clear the sink or outlive the adapter,
 * which it does by owning it.
 *
 * Thread affinity matches \c donner::gpu::Device: single-threaded use. Completion callbacks
 * touch only shared completion state, guarded independently of the adapter lifetime.
 */
class GeodeWgpuAdapterDevice final : public gpu::Device {
public:
  /**
   * Constructs a runtime device over \p root. Prefer \ref CreateGpuDeviceOver.
   *
   * @param root Backend root to render through; must not be null.
   */
  explicit GeodeWgpuAdapterDevice(std::shared_ptr<GeodeGpuRoot> root);

  /// The backend root this device renders through. Borrowed; the device retains it.
  const GeodeGpuRoot& root() const UTILS_LIFETIME_BOUND { return *root_; }

  /**
   * Installs the logical context this device's allocations and submissions are counted against.
   *
   * Non-owning, and cleared by passing null. The context owns this device, so it outlives the
   * attribution it installed.
   *
   * @param context Context to attribute to, or null to stop counting.
   */
  void setCounterSink(GeodeDevice* context) { counterSink_ = context; }

  /**
   * Polls the backend device, bracketed for ASYNCIFY suspend attribution.
   *
   * Under Emscripten, emdawnwebgpu implements `poll` by yielding the Asyncify-enabled thread for
   * roughly one browser task regardless of @p wait, so every poll unwinds and later rewinds the
   * wasm stack. With the whole application on one thread that wall time is UI frame time, so it
   * has to be attributable; route every poll through here rather than calling the backend
   * directly. The probe is a pair of clock reads on native builds, where `poll` does not suspend
   * at all.
   *
   * Prefer @p wait = false: a waiting poll can block inside a hung driver with no bound. Callers
   * that need to wait for submitted work should use \ref gpu::Device::waitForSerial, which is
   * bounded and reports a hang as a device-lost condition.
   *
   * @param wait Whether to let the backend block until pending work progresses.
   * @return True when the backend reports its queue empty (unspecified under Emscripten, where
   *   the poll is a browser-task yield).
   */
  bool pollSuspending(bool wait) const;

  /// Destructor; waits for in-flight submissions (so deferred destructions drain), then releases
  /// every wgpu object the adapter still owns.
  ~GeodeWgpuAdapterDevice() override;

  /// Serial of the most recent submission whose queue work-done callback has fired (0 if none).
  /// wgpu delivers the callbacks during \ref gpu::Device::waitForSerial's polling (and
  /// opportunistically on submit), so call it to guarantee progress.
  ///
  /// \warning The base class's `Device::poll()` does NOT drive wgpu polling - it only processes
  /// deferred destructions against the serial this method reports. Per-frame destroy+poll churn
  /// therefore defers unboundedly until something waits: a frame loop must call
  /// \ref gpu::Device::waitForSerial on its frame cadence (or extend the adapter with a
  /// non-blocking wgpu poll) so completions are observed and deferred destroys drain.
  /// Capped by \ref holdSubmittedWorkForTesting while a test holds submitted work incomplete.
  uint64_t completedSerial() const override;

  /**
   * Test seam: makes a wait slice behave as the browser's timed wait does when it expires
   * without the map completing - it reports that it handled the slice, having learned nothing.
   *
   * That arm is compiled out on every platform the test suites run on, so its contract is
   * otherwise checked only by the browser lane, which is exactly where it went unchecked.
   *
   * @param simulate Whether slices should take the simulated event-wait path.
   */
  void setSimulateEventWaitForTest(bool simulate) { simulateEventWaitForTest_ = simulate; }

  /**
   * Makes this device behave like one that accepted work and stopped retiring it.
   *
   * Test seam: a healthy device completes everything in microseconds, so the bounded waits and
   * their deadlines are otherwise unreachable. Both knobs describe real driver shapes, and the
   * waits must tell them apart: a driver that blocks in poll spends the wait's budget, while one
   * that returns from poll at once spends none of it and only exhausts the wait's own poll bound.
   *
   * @param completedSerialCeiling Highest serial \ref completedSerial may report;
   *   \ref kNoCompletedSerialCeiling restores what the backend reports.
   * @param pollCost Wall time each poll inside a serial wait costs on top of the backend's own,
   *   standing in for a poll that blocks until pending work progresses. Zero leaves the
   *   backend's poll timing alone.
   */
  void holdSubmittedWorkForTesting(uint64_t completedSerialCeiling,
                                   std::chrono::milliseconds pollCost) {
    completedSerialCeiling_.store(completedSerialCeiling, std::memory_order_relaxed);
    serialWaitPollCostMsForTesting_.store(pollCost.count(), std::memory_order_relaxed);
  }

  /// Ceiling value that leaves \ref completedSerial reporting what the backend reports.
  static constexpr uint64_t kNoCompletedSerialCeiling = std::numeric_limits<uint64_t>::max();

  /// Wall-clock budget the destructor spends draining submitted work before tearing down anyway.
  /// Lowered by tests that hold submitted work incomplete so the drain reaches its deadline
  /// without a multi-second wait. Test seam.
  ///
  /// @param seconds Budget in seconds.
  void setTeardownDrainBudgetForTesting(double seconds) { teardownDrainSeconds_ = seconds; }

  /**
   * TEMPORARY escape hatch (deleted with the presentation migration): registers an
   * externally owned wgpu texture - e.g. a render target created by the host or an earlier
   * non-migrated subsystem - as a \c donner::gpu::Texture of this adapter so migrated code can
   * reference it in render passes and copies. The adapter does NOT take ownership; destroying
   * the returned handle only forgets the registration, and
   * \ref gpu::Device::ownsTextureBacking reports false for it.
   *
   * The description is checked against the texture, so a caller that gets it wrong is refused
   * here rather than by the driver at the first pass that names it.
   *
   * @param texture Externally owned wgpu texture; must remain valid while registered.
   * @param size Texture extent in texels; must match \p texture.
   * @param format Texel format; must match \p texture.
   * @param usage Usage flags; \p texture must carry at least these.
   */
  gpu::Result<gpu::Texture> importExternalTexture(wgpu::Texture texture, const gpu::Extent2d& size,
                                                  gpu::TextureFormat format,
                                                  gpu::TextureUsage usage);

  /**
   * Registers a texture \p owner holds as a texture of this adapter, so code recording against
   * this adapter can name it.
   *
   * Two adapters driving the same backend device and queue are separate runtime devices, and a
   * texture of one is not a texture of the other: this registration is what makes it reachable
   * here. It is refused when \p owner drives a different backend device or queue - nothing this
   * adapter records could sample or copy that memory - and when \p texture is not a live texture
   * of \p owner, so a stale or forged handle cannot bridge whatever now occupies its slot. The
   * extent, format and capabilities come from \p owner's record of the texture, so a caller
   * cannot describe it differently than its owner does.
   *
   * This adapter does NOT take ownership: destroying the returned handle only forgets the
   * registration, and \p owner must keep the texture alive for as long as it is registered.
   *
   * @param owner Adapter that owns \p texture.
   * @param texture Live texture handle of \p owner.
   */
  gpu::Result<gpu::Texture> importTextureFrom(const GeodeWgpuAdapterDevice& owner,
                                              const gpu::Texture& texture);

  /**
   * TEMPORARY escape hatch (deleted with the presentation migration): the public form of this
   * adapter's handle-to-backend resolution, for the presentation call sites that still hand a
   * backend texture to something outside the runtime. Returns a null handle if \p texture does not
   * name a live texture of this adapter. Borrowed; the adapter (or the external owner) retains
   * ownership.
   *
   * @param texture Live texture handle of this adapter.
   */
  wgpu::Texture wgpuTextureOf(const gpu::Texture& texture) const;

  /**
   * TEMPORARY escape hatch (deleted with the readback and presentation migration): returns the
   * wgpu texture view behind \p textureView, or a null handle if unknown. Borrowed.
   *
   * @param textureView Live texture view handle of this adapter.
   */
  wgpu::TextureView wgpuTextureViewOf(const gpu::TextureView& textureView) const;

protected:
  gpu::Status onCreateSurface(uint32_t slotIndex,
                              const gpu::SurfaceDescriptor& descriptor) override;
  gpu::Result<gpu::SurfaceCapabilities> onSurfaceCapabilities(uint32_t slotIndex) const override;
  gpu::Status onConfigureSurface(uint32_t slotIndex,
                                 const gpu::SurfaceConfiguration& configuration) override;
  gpu::Result<gpu::SurfaceStatus> onAcquireCurrentTexture(uint32_t slotIndex,
                                                          uint32_t textureSlotIndex) override;
  gpu::Result<gpu::SurfaceStatus> onPresentSurface(uint32_t slotIndex) override;
  void onAbandonCurrentTexture(uint32_t slotIndex) override;
  void onDestroySurface(uint32_t slotIndex) override;

  gpu::Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex,
                               gpu::MapMode mode, uint64_t offsetBytes,
                               uint64_t byteCount) override;
  gpu::MapSliceReport onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) override;

  /**
   * Drives `wgpu::Device::poll` until \ref completedSerial reaches \p serial, the device is
   * lost, or the budget elapses. On Emscripten the poll shim yields through Asyncify, mirroring
   * \ref GeodeDevice's wait machinery. This is the only entry point that drives wgpu polling for
   * this adapter - see the warning on \ref completedSerial.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds.
   */
  bool onWaitForSerial(uint64_t serial, double timeoutSeconds) override;

private:
  /**
   * Names \p backend in a fresh texture slot of this adapter, describing it with \p descriptor.
   *
   * This is the one mechanism by which a texture this adapter did not allocate becomes nameable
   * here: the slot holds a borrowed alias, \ref onOwnsTextureBacking reports false for it, and
   * destroying the handle only forgets the registration.
   *
   * Refused when \p descriptor does not describe \p backend, because nothing downstream re-reads
   * the backend and a record that misdescribes its texture is only discovered by the driver.
   *
   * @param backend Backend texture to name; must remain valid while the registration is live.
   * @param descriptor How the registration describes it; must match \p backend.
   */
  gpu::Result<gpu::Texture> registerBorrowedTexture(wgpu::Texture backend,
                                                    const gpu::TextureDescriptor& descriptor);

  /**
   * The backend texture \p texture names, or a null handle when it does not name a live texture
   * of this adapter. Validation is the full handle check (null, device identity, and generation),
   * so a stale or forged handle cannot reach the slot's new occupant.
   *
   * @param texture Texture handle to resolve.
   */
  wgpu::Texture liveBackendTexture(const gpu::Texture& texture) const;

  /// Second bound on \ref waitForSerialBounded, for a driver whose poll returns without either
  /// progressing or costing wall time. It keeps such a wait from spinning a core; it is not a
  /// deadline, and reaching it with the budget unspent says nothing about the device.
  static constexpr int kMaxSerialWaitPolls = 20000;

  /// Whether a wait that spends its whole budget without the work retiring declares the backend
  /// root lost.
  enum class LossOnTimeout {
    /// A caller's own bounded wait: spending the budget is the observation it was there to make,
    /// so publish it with the attribution that makes the report diagnosable.
    Declare,
    /// Teardown's drain: it already proceeds on timeout, it is nobody's deadline, and the other
    /// contexts over this root are still rendering through it.
    Tolerate,
  };

  /// Drives \ref pollForSerialCompletion until \ref completedSerial reaches \p serial, the
  /// device is lost, the budget elapses, or the poll bound above is reached.
  ///
  /// @param serial Submission serial to wait for.
  /// @param timeoutSeconds Longest to wait, in seconds.
  /// @param onTimeout What a wait that spends its whole budget publishes.
  /// @return True once this device has completed \p serial.
  bool waitForSerialBounded(uint64_t serial, double timeoutSeconds, LossOnTimeout onTimeout);

  /**
   * Reports one counted event to the logical context installed as this device's counter sink.
   *
   * One null check in one place: the sink is absent for a device nothing has claimed yet, and
   * every counted site would otherwise repeat the same guard.
   *
   * @param report Counting member of \ref GeodeDevice to call.
   * @param args Arguments that member takes.
   */
  template <typename Report, typename... Args>
  void count(Report report, Args... args) const {
    if (counterSink_ != nullptr) {
      (counterSink_->*report)(args...);
    }
  }

  /// Ends a serial wait that observed no completion, declaring the backend root lost when the
  /// wait had a real budget to spend.
  ///
  /// @param start When the wait began, so the report carries what it actually spent rather than
  ///   the budget it was given.
  /// @param timeoutSeconds Budget the wait was given; zero means the caller asked what was
  ///   already known rather than waiting, so the negative answer declares nothing.
  /// @param onTimeout What this wait publishes; \ref LossOnTimeout::Tolerate declares nothing.
  /// @return False, always: the wait did not observe the serial complete.
  bool giveUpOnSerialWait(std::chrono::steady_clock::time_point start, double timeoutSeconds,
                          LossOnTimeout onTimeout);

  /// One iteration of \ref onWaitForSerial's wait: lets the backend block until pending work
  /// progresses, plus whatever extra cost \ref holdSubmittedWorkForTesting gave that poll.
  void pollForSerialCompletion();

  /// Highest serial \ref completedSerial may report; see \ref holdSubmittedWorkForTesting.
  std::atomic<uint64_t> completedSerialCeiling_{kNoCompletedSerialCeiling};

  /// Wall time each poll inside a serial wait costs on top of the backend's own, in milliseconds;
  /// see \ref holdSubmittedWorkForTesting.
  std::atomic<std::chrono::milliseconds::rep> serialWaitPollCostMsForTesting_{0};

  /// Budget \ref ~GeodeWgpuAdapterDevice spends draining submitted work. Generous: a healthy
  /// device drains in microseconds, so it only trips on a driver that has effectively hung, and
  /// teardown proceeds either way.
  double teardownDrainSeconds_ = 5.0;

protected:
  /// Destroys the wgpu buffer in \p slotIndex, so the allocation goes back now rather than when
  /// the host runtime next collects. @param slotIndex Validated live buffer slot.
  void onDestroyBufferBacking(uint32_t slotIndex) override;

  /// Destroys the wgpu texture in \p slotIndex if this adapter allocated it; an external
  /// registration belongs to the embedder and is left alone.
  /// @param slotIndex Validated live texture slot.
  void onDestroyTextureBacking(uint32_t slotIndex) override;

  /// Whether \p slotIndex holds a texture this adapter allocated, rather than a borrowed one
  /// named through \ref registerBorrowedTexture. @param slotIndex Validated live texture slot.
  [[nodiscard]] bool onOwnsTextureBacking(uint32_t slotIndex) const override;
  gpu::Result<std::span<const uint8_t>> onMappedBytes(uint32_t mappingSlotIndex) const override;
  void onUnmapBuffer(uint32_t mappingSlotIndex) override;

  gpu::Status onCreateBuffer(uint32_t slotIndex, const gpu::BufferDescriptor& descriptor) override;
  gpu::Status onCreateTexture(uint32_t slotIndex,
                              const gpu::TextureDescriptor& descriptor) override;
  gpu::Status onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                  const gpu::TextureViewDescriptor& descriptor) override;
  gpu::Status onCreateSampler(uint32_t slotIndex,
                              const gpu::SamplerDescriptor& descriptor) override;
  gpu::Status onCreateBindGroupLayout(uint32_t slotIndex,
                                      const gpu::BindGroupLayoutDescriptor& descriptor) override;
  gpu::Status onCreateBindGroup(uint32_t slotIndex,
                                const gpu::BindGroupDescriptor& descriptor) override;
  gpu::Status onCreatePipelineLayout(uint32_t slotIndex,
                                     const gpu::PipelineLayoutDescriptor& descriptor) override;
  gpu::Status onCreateShaderModule(uint32_t slotIndex,
                                   const gpu::ShaderModuleDescriptor& descriptor) override;
  gpu::Status onCreateRenderPipeline(uint32_t slotIndex,
                                     const gpu::RenderPipelineDescriptor& descriptor) override;
  gpu::Status onCreateComputePipeline(uint32_t slotIndex,
                                      const gpu::ComputePipelineDescriptor& descriptor) override;
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override;
  gpu::Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                            std::span<const uint8_t> data) override;
  gpu::Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                             const gpu::TexelCopyBufferLayout& dataLayout,
                             const gpu::Extent2d& writeSize,
                             const gpu::Origin2d& destinationOrigin) override;
  gpu::Status onSubmit(uint64_t submissionSerial,
                       std::span<const gpu::SubmittedCommandBuffer> commandBuffers) override;

private:
  /// One texture slot: the borrowed alias used for encoding, plus a +1 owning reference when
  /// the adapter created the texture (empty for imported external textures).
  struct TextureSlot {
    ScopedWgpuHandle<wgpu::Texture> ownedTexture;  //!< Owned +1 reference (adapter-created only).
    wgpu::Texture texture;                         //!< Borrowed alias; null when the slot is dead.
  };

  friend struct GeodeWgpuAdapterDeviceTestAccess;

  /// State retained by callbacks after their adapter may have been destroyed.
  struct CompletionState {
    /// One queued submission, named by the ticket its completion callback retains.
    struct Pending {
      uint64_t firstSerial = 0;  //!< Unique ticket retained by its completion callback.
      uint64_t lastSerial = 0;   //!< Highest serial in this submission.
    };

    /// Records a queued submission. @param serial Logical serial.
    void record(uint64_t serial);
    /// Completes exactly one queued range and publishes the contiguous completed prefix.
    /// @param ticket Unique first serial of the completed range.
    void complete(uint64_t ticket);

    std::atomic<uint64_t> completedSerial{0};  //!< Highest serial with no unfinished predecessor.
    std::mutex mutex;                 //!< Protects ranges and their completed high-water mark.
    SmallVector<Pending, 4> pending;  //!< Includes queued ranges until their callbacks run.
    uint64_t completedHighWater = 0;  //!< Highest serial seen by a completed callback.
  };

  /// Mutable state threaded through the encoding of one command stream.
  struct EncodingState {
    ScopedWgpuHandle<wgpu::CommandEncoder> encoder;          //!< Encoder this adapter records into.
    ScopedWgpuHandle<wgpu::RenderPassEncoder> pass;          //!< Active render pass, or empty.
    ScopedWgpuHandle<wgpu::ComputePassEncoder> computePass;  //!< Active compute pass, or empty.
  };

  /// Opens a render pass with the recorded color attachments.
  /// @param state Encoding state.
  /// @param beginPass Recorded command.
  gpu::Status encodeBeginRenderPass(EncodingState& state,
                                    const gpu::BeginRenderPassCommand& beginPass);
  /// Binds a recorded pipeline.
  /// @param state Encoding state.
  /// @param setPipeline Recorded command.
  gpu::Status encodeSetPipeline(EncodingState& state, const gpu::SetPipelineCommand& setPipeline);
  /// Binds a recorded bind group.
  /// @param state Encoding state.
  /// @param setBindGroup Recorded command.
  gpu::Status encodeSetBindGroup(EncodingState& state,
                                 const gpu::SetBindGroupCommand& setBindGroup);
  /// Binds a recorded vertex buffer.
  /// @param state Encoding state.
  /// @param setVertexBuffer Recorded command.
  gpu::Status encodeSetVertexBuffer(EncodingState& state,
                                    const gpu::SetVertexBufferCommand& setVertexBuffer);
  /// Binds a recorded index buffer.
  /// @param state Encoding state.
  /// @param setIndexBuffer Recorded command.
  gpu::Status encodeSetIndexBuffer(EncodingState& state,
                                   const gpu::SetIndexBufferCommand& setIndexBuffer);
  /// Sets an explicit scissor rectangle.
  /// @param state Encoding state.
  /// @param setScissor Recorded command.
  gpu::Status encodeSetScissorRect(EncodingState& state,
                                   const gpu::SetScissorRectCommand& setScissor);
  /// Sets an explicit viewport.
  /// @param state Encoding state.
  /// @param setViewport Recorded command.
  gpu::Status encodeSetViewport(EncodingState& state, const gpu::SetViewportCommand& setViewport);
  /// Issues a draw.
  /// @param state Encoding state.
  /// @param draw Recorded command.
  gpu::Status encodeDraw(EncodingState& state, const gpu::DrawCommand& draw);
  /// Issues an indexed draw.
  /// @param state Encoding state.
  /// @param draw Recorded command.
  gpu::Status encodeDrawIndexed(EncodingState& state, const gpu::DrawIndexedCommand& draw);
  /// Ends the active render pass.
  /// @param state Encoding state.
  gpu::Status encodeEndRenderPass(EncodingState& state);
  /// Opens a compute pass.
  /// @param state Encoding state.
  /// @param beginPass Recorded command.
  gpu::Status encodeBeginComputePass(EncodingState& state,
                                     const gpu::BeginComputePassCommand& beginPass);
  /// Binds a recorded compute pipeline.
  /// @param state Encoding state.
  /// @param setPipeline Recorded command.
  gpu::Status encodeSetComputePipeline(EncodingState& state,
                                       const gpu::SetComputePipelineCommand& setPipeline);
  /// Issues a dispatch.
  /// @param state Encoding state.
  /// @param dispatch Recorded command.
  gpu::Status encodeDispatchWorkgroups(EncodingState& state,
                                       const gpu::DispatchWorkgroupsCommand& dispatch);
  /// Ends the active compute pass.
  /// @param state Encoding state.
  gpu::Status encodeEndComputePass(EncodingState& state);
  /// Records a texture-to-buffer copy.
  /// @param state Encoding state.
  /// @param copy Recorded command.
  gpu::Status encodeCopyTextureToBuffer(EncodingState& state,
                                        const gpu::CopyTextureToBufferCommand& copy);
  /// Records a texture-to-texture copy.
  /// @param state Encoding state.
  /// @param textureCopy Recorded command.
  gpu::Status encodeCopyTextureToTexture(EncodingState& state,
                                         const gpu::CopyTextureToTextureCommand& textureCopy);
  /// Encodes one recorded command through the exhaustive command-variant dispatch.
  /// @param state Encoding state.
  /// @param command Recorded command.
  gpu::Status encodeCommand(EncodingState& state, const gpu::Command& command);

  /// Records one submitted command buffer into \p state's encoder, leaving it unfinished.
  /// @param state Encoding state whose encoder is already open.
  /// @param commands Commands to record, in recording order.
  gpu::Status encodeSubmittedCommandBuffer(EncodingState& state,
                                           std::span<const gpu::Command> commands);

  /// Clears the slot of one non-pipeline resource kind, or returns false when the name is not
  /// one of them.
  /// @param resourceName Resource type name. @param slotIndex Slot to clear.
  bool clearResourceSlot(std::string_view resourceName, uint32_t slotIndex);

  /// Clears the slot of one pipeline-family resource kind, or returns false when the name is not
  /// one of them.
  /// @param resourceName Resource type name. @param slotIndex Slot to clear.
  bool clearPipelineSlot(std::string_view resourceName, uint32_t slotIndex);

  /// Attaches a queue callback retaining only shared completion state and a unique ticket.
  /// @param ticket First serial of the queued or discarded range.
  void completeWhenQueueDrains(uint64_t ticket);

  /// Declared before every slot vector so the backend handles outlive the objects created from
  /// them: members are destroyed in reverse declaration order.
  std::shared_ptr<GeodeGpuRoot> root_;

  /// Logical context this device's allocations and submissions are counted against, or null.
  /// Non-owning; see \ref setCounterSink.
  GeodeDevice* counterSink_ = nullptr;

  /// State of one pending or completed host mapping.
  ///
  /// The completion flag is shared with the backend's callback, which may fire from inside any
  /// call into the backend, so it is atomic and outlives this record through a reference count
  /// the callback also holds.
  struct MappingSlot {
    struct Completion {
      std::atomic<int> references{2};  //!< This record and the pending callback.
      std::atomic<bool> done{false};   //!< Set once the callback has run.
      std::atomic<bool> ok{false};     //!< Whether the map succeeded.
      /// Set once the mapping handle is gone, which makes whichever side observes the finished
      /// map responsible for giving the buffer back.
      std::atomic<bool> abandoned{false};
      /// Claimed once by whichever side unmaps, so the two never both unmap and never both
      /// leave it to the other.
      std::atomic<bool> unmapClaimed{false};
      /// The buffer being mapped, holding a reference of its own: a map abandoned in flight
      /// completes after the mapping's slot is gone and must still have a buffer to unmap.
      wgpu::Buffer buffer;

      /// Gives the buffer back once the mapping is gone and the map has succeeded.
      ///
      /// Both the release and the completion call this, because either can be the one that sees
      /// both conditions hold. A buffer left mapped with nothing able to unmap it stays mapped
      /// for the rest of its life, and every later GPU use of it is invalid.
      void unmapIfAbandoned() {
        if (!abandoned.load(std::memory_order_acquire) || !done.load(std::memory_order_acquire) ||
            !ok.load(std::memory_order_relaxed)) {
          return;
        }
        if (!unmapClaimed.exchange(true, std::memory_order_acq_rel) && buffer) {
          buffer.unmap();
        }
      }

      /// Drops one reference, deleting the state with the last one.
      void release() {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          if (buffer) {
            buffer.release();
          }
          delete this;
        }
      }
    };

    Completion* completion = nullptr;  //!< Shared completion state, or null for a dead slot.
    wgpu::Buffer buffer;               //!< Buffer being mapped; borrowed from its slot.
    uint64_t offsetBytes = 0;          //!< Byte offset of the mapped range.
    uint64_t byteCount = 0;            //!< Length of the mapped range.
    /// Future the map request returned, so a wait slice can wait on the completion event itself
    /// where the platform supports it rather than polling for it.
    wgpu::Future mapFuture{};
  };

  /// The mapping's state right now: Ready or Failed once the map has completed, DeviceLost on a
  /// lost device, and Pending until then.
  /// @param completion Completion state to read.
  gpu::MapSliceState sliceStateOf(const MappingSlot::Completion& completion) const;

  /// Waits out one slice on the map's completion event where the platform supports it.
  /// @param mappingSlotIndex Slot of the mapping to wait on.
  /// @param slice Length of this wait slice.
  /// @return True if the slice was waited on the event (so the caller re-reads the completion
  ///   rather than polling), false if this platform or this thread cannot event-wait it.
  bool waitOnMapFutureSlice(uint32_t mappingSlotIndex, std::chrono::microseconds slice);

  /// Applies a completed event-wait slice through the same path on browsers and in tests.
  bool finishMapWaitSlice(uint32_t mappingSlotIndex, const MappingSlot::Completion* completion,
                          wgpu::Future future, wgpu::WaitStatus status);

  /// Revalidates a slot after a backend wait may have yielded to other work.
  bool mappingStillMatches(uint32_t mappingSlotIndex, const MappingSlot::Completion* completion,
                           wgpu::Future future) const;

  /// Test-only replacement for the suspending backend wait; may exercise reentrant slot changes.
  std::function<wgpu::WaitStatus()> timedMapWaitForTest_;

  /// Makes \ref waitOnMapFutureSlice report that an event wait handled the slice without one
  /// having happened, so the browser arm's contract can be checked where that arm is compiled
  /// out. See \ref simulateEventWaitForTest.
  bool simulateEventWaitForTest_ = false;

  /// One presentation surface and the texture it has handed out this frame.
  struct SurfaceSlot {
    ScopedWgpuHandle<wgpu::Surface> surface;  //!< Owned surface, or null for a dead slot.
    /// Texture the surface handed out for the current frame; borrowed, since the surface owns it.
    wgpu::Texture acquired;
    /// Slot the runtime gave that texture, so abandoning can clear the same one.
    uint32_t acquiredTextureSlot = 0;
    bool hasAcquired = false;  //!< Whether \ref acquired names this frame's texture.
  };

  std::vector<SurfaceSlot> slotSurfaces_;

  std::vector<MappingSlot> slotMappings_;

  std::vector<ScopedWgpuHandle<wgpu::Buffer>> slotBuffers_;
  std::vector<TextureSlot> slotTextures_;
  std::vector<ScopedWgpuHandle<wgpu::TextureView>> slotTextureViews_;
  std::vector<ScopedWgpuHandle<wgpu::Sampler>> slotSamplers_;
  std::vector<ScopedWgpuHandle<wgpu::BindGroupLayout>> slotBindGroupLayouts_;
  std::vector<ScopedWgpuHandle<wgpu::BindGroup>> slotBindGroups_;
  std::vector<ScopedWgpuHandle<wgpu::PipelineLayout>> slotPipelineLayouts_;
  std::vector<ScopedWgpuHandle<wgpu::ShaderModule>> slotShaderModules_;
  std::vector<ScopedWgpuHandle<wgpu::RenderPipeline>> slotRenderPipelines_;
  std::vector<ScopedWgpuHandle<wgpu::ComputePipeline>> slotComputePipelines_;

  std::shared_ptr<CompletionState> completionState_ = std::make_shared<CompletionState>();

  /// Set only inside \ref registerBorrowedTexture so \ref onCreateTexture names that texture in
  /// the slot instead of allocating one.
  wgpu::Texture pendingRegistration_;
};

/**
 * Maps a wgpu render-target format onto the \c donner::gpu format enum. Halts (release assert)
 * on formats outside the runtime's supported set - RGBA8Unorm, BGRA8Unorm, R8Unorm - which are
 * the only formats Geode's shaders and render targets are built for.
 *
 * @param format wgpu texture format to map.
 */
gpu::TextureFormat GpuTextureFormatFromWgpu(wgpu::TextureFormat format);

/**
 * Maps a \c donner::gpu render-target format onto the wgpu format enum, for the call sites that
 * still describe a backend texture. Halts (release assert) on a format the backend cannot name.
 *
 * @param format Runtime texture format to map.
 */
wgpu::TextureFormat WgpuTextureFormatFrom(gpu::TextureFormat format);

/**
 * Maps wgpu texture usage flags onto the \c donner::gpu usage flags. Flags with no runtime
 * equivalent are dropped, so the result describes exactly the capabilities the runtime can
 * express for the texture.
 *
 * @param usage wgpu usage flags to map.
 */
gpu::TextureUsage GpuTextureUsageFromWgpu(wgpu::TextureUsage usage);

}  // namespace donner::geode
