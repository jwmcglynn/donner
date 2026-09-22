#pragma once
/// @file
/// \c donner::gpu::metal::MetalDevice - the Metal backend for the Donner GPU runtime.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "donner/gpu/Device.h"
#include "donner/gpu/GpuLimits.h"

namespace donner::gpu::metal {

/**
 * Metal backend of the Donner GPU runtime.
 *
 * Inherits every fail-closed validation check from \ref donner::gpu::Device; the `on*` hooks
 * receive only validated input and translate it to Metal objects. Any Metal-side failure (nil
 * object, compile error, encoder failure) fails closed with a \ref donner::gpu::GpuError; the
 * backend never crashes on such failures.
 *
 * Scope: host-visible buffers and textures, MSL shader modules, render pipelines with up to
 * \ref kMaxVertexBuffers vertex buffer layouts stepped per vertex or per instance, render passes
 * with color attachments, scissors and viewports, non-indexed and indexed (16- and 32-bit) draws
 * with instancing and base vertex, compute pipelines and passes over sampled and storage
 * textures, texture-to-buffer and texture-to-texture copies, queue writes, host buffer mapping,
 * and surface presentation. Resources bind through bind group 0 only; a submission that sets any
 * other group is refused. Bindings follow the deterministic argument-table mapping in
 * `donner/gpu/shader/MslBindingMap.h`: buffer binding `b` maps to Metal buffer index `1 + b`, and
 * texture and sampler bindings map directly. Vertex buffer layouts take the highest free
 * vertex-stage argument indices at or below 30, in slot order, skipping the length table and every
 * vertex-visible resource buffer; a pipeline whose vertex buffers do not fit is refused at
 * creation.
 *
 * Generated runtime-array reads use the exact declared binding sizes in a fixed length table.
 * Metal buffer index 0 is reserved for that table, so raw MSL this backend accepts must leave
 * index 0 free. Each immutable bind group caches the table; when the bound group changes it is
 * uploaded to every stage the active encoder has, deliberately without consulting the layout,
 * because the generated code's need for the table follows from the shader IR rather than from a
 * layout's binding types or visibility. Repeated draws and dispatches reuse that binding, and
 * each new pass binds it again. MSL modules must supply buffer interface metadata before native
 * compilation, including an explicitly empty list when no entry point uses buffers. Pipeline
 * creation checks the supplied facts against the layout's binding types and stage visibility.
 *
 * Presentation goes through a Core Animation Metal layer the embedder supplies and continues to
 * own: configuring sets the layer's pixel format, drawable extent, pacing and alpha compositing,
 * and each acquired frame is one of that layer's drawables, handed out only while it matches the
 * configured extent. Presenting waits for the frame's own submission to complete and then hands
 * the drawable over, because a drawable presented from a command buffer is shown when that
 * buffer is scheduled rather than when it completes, which would show a frame the GPU is still
 * drawing. The layer hands out a small fixed number of drawables, so a frame that is neither
 * presented nor abandoned stalls the next acquisition until the layer gives up waiting.
 *
 * Queue writes update idle resources directly. Writes to resources an earlier submission still
 * uses are copied into bounded host storage and uploaded at the beginning of the next ordinary
 * submission. Repeated writes of the same resource range are coalesced. A batch adds one staging
 * allocation and no queue submission; Metal retains the staging buffer and destinations until
 * completion. Queued and in-flight uploads share a configurable byte budget, defaulting to
 * \ref kMaxBufferByteSize; at most 16,384 writes may await submission. Excess writes return
 * `LimitExceeded` without changing the batch. The budget counts logical upload bytes, excluding
 * row and alignment padding. Combined queued and in-flight packed staging also stays within
 * the larger of this budget and \ref kMaxBufferByteSize. Packing temporarily retains both the
 * host payload and native staging buffer. Managed resources may
 * have separate host and device backing, so physical RAM/VRAM usage also includes those copies
 * and driver overhead. Destroyed destinations drop unsubmitted writes; completion returns the
 * in-flight byte reservation.
 *
 * macOS buffer blits require four-byte-aligned offsets and sizes. An unaligned update instead
 * waits at most five seconds for that buffer's last use, returning an error on timeout; it never
 * waits for unrelated later submissions.
 *
 * Memory model: unified-memory resources use `MTLStorageModeShared`; other devices use
 * `MTLStorageModeManaged`. Managed CPU writes publish their ranges, and every submission
 * synchronizes its GPU-written buffers before reporting completion to host readers. Textures
 * reach the CPU through readback buffers, so unrelated buffers and textures need no scan.
 *
 * Threading: single-threaded use, matching \ref donner::gpu::Device's thread affinity. The one
 * exception is command-buffer completion handlers, which Metal invokes on an internal queue;
 * they touch only atomics, a mutex-protected error string, and the root's shared loss condition,
 * observable through \ref completedSerial, \ref Device::waitForSerial, \ref Device::isLost, and
 * \ref lastErrorForTest.
 *
 * A command buffer that fails on the GPU declares the root lost, with no wait site because the
 * backend reported it, before its serial is reported complete. Mappings answer loss before
 * readiness, whichever device over the root declared it.
 *
 * The header is pure C++ (Objective-C state lives behind a pimpl) so it is includable from C++
 * tests; the implementation is Objective-C++.
 */
class MetalDevice final : public Device {
public:
  /// Native shader representation accepted by this device.
  ShaderSourceKind shaderSourceKind() const override { return ShaderSourceKind::Msl; }

  /// Which memory model the backend builds its resources for.
  enum class MemoryModel : uint8_t {
    /// Take the model the Metal device reports. Production always uses this.
    Detected,
    /// Build for a device without unified memory whatever it reports, so the host-coherency
    /// steps that model needs are exercised on hardware that would otherwise never take them.
    ForceNonUnified,
  };

  /// What the system default Metal device supports, which every device \ref Create opens shares.
  struct SystemCapabilities {
    /// Largest width or height of a 2D texture the device allocates, from its GPU family and
    /// capped at \ref kMaxTextureDimension, the largest extent the runtime accepts.
    uint32_t maxTextureDimension2D = 0;
  };

  /// The GPU families a Metal device belongs to, as far as its 2D texture limit depends on them.
  struct GpuFamilies {
    /// Any Mac family. Every Metal device on macOS is in one, Apple silicon included.
    bool mac = false;
    /// Apple family 3 or later.
    bool apple3OrLater = false;
  };

  /**
   * Largest width or height of a 2D texture a device of \p families allocates, from Metal's
   * feature set tables: 16,384 for every Mac family and for Apple family 3 onward, and 8,192 for
   * the earlier Apple families. Capped at \ref kMaxTextureDimension.
   *
   * @param families Families the device belongs to.
   */
  static uint32_t MaxTextureDimension2DFor(GpuFamilies families);

  /**
   * Asks the system default Metal device, the one \ref Create opens, what it supports without
   * opening a runtime device over it.
   *
   * A backend root is selected before any device over it exists, and its limits have to be the
   * device's own rather than a portable fallback.
   *
   * @return The capabilities, or empty when no Metal device is available.
   */
  static std::optional<SystemCapabilities> QuerySystemCapabilities();

  /**
   * Creates a device on the system default Metal device. Returns nullptr if no Metal device is
   * available (for example on a CI host without a GPU).
   *
   * @param memoryModel Which memory model to build resources for; production leaves this
   *   detected, and a test forces the non-unified path to cover it on unified hardware.
   * @param uploadStagingByteBudget Maximum combined queued and in-flight logical payload bytes.
   *   Zero is invalid and returns nullptr. Each individual batch also fits \ref kMaxBufferByteSize.
   * @param unalignedWriteTimeout Maximum CPU wait for an unaligned write to a busy buffer.
   *   Must be between zero and five seconds; invalid budgets return nullptr.
   * @param lostState Loss condition to share with every other device selected over the same
   *   backend, or null for a private one only this device can set. The device reports it through
   *   \ref Device::isLost; the loss is declared by whoever observes it, such as a failed command
   *   buffer of this device or a context's bounded wait.
   */
  static std::unique_ptr<MetalDevice> Create(
      MemoryModel memoryModel = MemoryModel::Detected,
      uint64_t uploadStagingByteBudget = kMaxBufferByteSize,
      std::chrono::milliseconds unalignedWriteTimeout = std::chrono::seconds(5),
      std::shared_ptr<DeviceLostState> lostState = nullptr);

  /// Whether this device's resources are built for unified memory. Test accessor.
  [[nodiscard]] bool usesUnifiedMemoryForTest() const;

  /// How many buffer writes have published their range to the device copy. Test accessor.
  ///
  /// Counts CPU-written buffer ranges, including packed staging for buffer or texture uploads.
  /// Direct texture writes publish through `replaceRegion` and need no buffer-range publication.
  ///
  /// On unified memory nothing is published and this stays zero. The count exists because
  /// hardware that addresses one copy produces correct results whether or not the publication
  /// happened, so the results cannot show whether it did.
  [[nodiscard]] uint64_t hostWritePublishCountForTest() const;

  /// How many submissions have published the device's changes back to the host copy. Test
  /// accessor, for the same reason as above but in the other direction.
  [[nodiscard]] uint64_t deviceWritePublishCountForTest() const;

  /// Snapshot of upload bookkeeping for tests and performance measurements.
  struct WriteStats {
    size_t pendingWrites = 0;             //!< Coalesced writes awaiting a normal submission.
    uint64_t inFlightStagingBytes = 0;    //!< Packed staging bytes retained until GPU completion.
    uint64_t pendingStagingBytes = 0;     //!< Packed staging bytes reserved for those writes.
    uint64_t stagingAllocations = 0;      //!< Native upload staging allocations attempted.
    uint64_t submittedUploadBatches = 0;  //!< Ordinary submissions containing queued writes.
    uint64_t unalignedWriteWaits = 0;     //!< Resource waits needed by unaligned buffer writes.
  };

  /// Current queue-write counters. Does not wait or change device state.
  WriteStats writeStatsForTest() const;

  /// Pauses submitted GPU work at a shared event until \ref resumeSubmissionsForTest is called.
  /// A deterministic test seam for writes issued while an earlier submission is in flight.
  Status pauseSubmissionsForTest();

  /// Releases the event installed by \ref pauseSubmissionsForTest. Safe when no pause is active.
  void resumeSubmissionsForTest();

  /**
   * Makes one command buffer of the next submission report an execution error when it completes,
   * as a command buffer the GPU faulted on does, through the same completion path. A deterministic
   * test seam: a real fault cannot be produced on demand without hanging or corrupting the GPU.
   *
   * @param commandBufferIndex Index of the failing buffer within the submission; its last buffer
   *   when absent or past the end.
   */
  void failNextSubmissionForTest(std::optional<size_t> commandBufferIndex = std::nullopt);

  /// Parks the completion of the next submission when its handler runs, as a GPU that has not
  /// finished that work yet looks: the handler records its outcome but publishes nothing until
  /// \ref releaseHeldCompletionForTest. Later submissions complete normally meanwhile.
  void holdNextCompletionForTest();

  /// Publishes the completion \ref holdNextCompletionForTest parked, from the calling thread, or
  /// lets it publish normally when its handler has not run yet. Safe when nothing is held.
  void releaseHeldCompletionForTest();

  /// Waits until \p count completion handlers have run on this device, including parked ones.
  /// Test seam for ordering completions deterministically.
  /// @param count Handlers to wait for. @param timeoutSeconds Longest to wait.
  /// @return True once that many have run.
  [[nodiscard]] bool waitForCompletionHandlersForTest(uint64_t count, double timeoutSeconds) const;

  /// Destructor; releases all Metal objects still alive.
  ~MetalDevice() override;

  /// Serial of the most recent submission whose Metal command buffer has completed on the GPU
  /// (0 if none). Updated by completion handlers, which may run on another thread.
  uint64_t completedSerial() const override;

  /**
   * Copies the full contents of \p buffer back to the host and returns the bytes.
   *
   * Test/readback convenience, pending a buffer mapping API: it validates device identity, slot
   * liveness, and the handle generation, then reads
   * the shared-storage Metal buffer contents directly. Callers must ensure relevant GPU work has
   * completed first (see \ref Device::waitForSerial). Queued writes become visible after a
   * subsequent ordinary submission completes; this accessor does not submit them.
   *
   * @param buffer Buffer to read back; must be a live buffer of this device.
   */
  Result<std::vector<uint8_t>> readBackBuffer(const Buffer& buffer);

  /// Native texture access reported by Metal, exposed for allocation-contract tests.
  struct NativeTextureUsage {
    bool shaderRead = false;    //!< Native shader-read usage.
    bool shaderWrite = false;   //!< Native shader-write usage.
    bool renderTarget = false;  //!< Native render-target usage.
  };

  /// Reads native usage after validating the texture's device, liveness, and generation.
  /// @param texture A live texture owned by this device.
  Result<NativeTextureUsage> textureUsageForTest(const Texture& texture) const;

  /// Message of the most recent asynchronous command-buffer execution error captured by a
  /// completion handler, or an empty string if none occurred. Test/diagnostic accessor.
  std::string lastErrorForTest() const;

  /// Name the Metal driver reports for the underlying device, for example `Apple M1 Pro`. Two
  /// GPUs running the same shaders can round a covered edge texel differently, so anything
  /// comparing this backend's pixels against a committed record has to know which one it is on.
  std::string adapterName() const;

protected:
  Status onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) override;
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) override;
  Status onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                             const TextureViewDescriptor& descriptor) override;
  Status onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) override;
  Status onCreateBindGroupLayout(uint32_t slotIndex,
                                 const BindGroupLayoutDescriptor& descriptor) override;
  Status onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) override;
  Status onCreatePipelineLayout(uint32_t slotIndex,
                                const PipelineLayoutDescriptor& descriptor) override;
  Status onCreateShaderModule(uint32_t slotIndex,
                              const ShaderModuleDescriptor& descriptor) override;
  Status onCreateRenderPipeline(uint32_t slotIndex,
                                const RenderPipelineDescriptor& descriptor) override;
  Status onCreateComputePipeline(uint32_t slotIndex,
                                 const ComputePipelineDescriptor& descriptor) override;
  void onRetireBuffer(uint32_t slotIndex) override;
  void onRetireTexture(uint32_t slotIndex) override;
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override;
  Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                       std::span<const uint8_t> data) override;
  Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                        const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize,
                        const Origin2d& destinationOrigin) override;
  Status onSubmit(uint64_t submissionSerial,
                  std::span<const SubmittedCommandBuffer> commandBuffers) override;
  Status onCreateSurface(uint32_t slotIndex, const SurfaceDescriptor& descriptor) override;
  Result<SurfaceCapabilities> onSurfaceCapabilities(uint32_t slotIndex) const override;
  Status onConfigureSurface(uint32_t slotIndex, const SurfaceConfiguration& configuration) override;
  Result<SurfaceStatus> onAcquireCurrentTexture(uint32_t slotIndex,
                                                uint32_t textureSlotIndex) override;
  Result<SurfaceStatus> onPresentSurface(uint32_t slotIndex) override;
  void onAbandonCurrentTexture(uint32_t slotIndex) override;
  void onDestroySurface(uint32_t slotIndex) override;

  // Host buffer mapping. Every buffer is already host-visible, so a mapping is the wait for the
  // work that fills it plus a bounds-checked view of its contents; the shared mapping table owns
  // that bookkeeping and this backend answers only the Metal-specific facts.
  Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex, MapMode mode,
                          uint64_t offsetBytes, uint64_t byteCount) override;
  MapSliceReport onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) override;

  /**
   * Polls the completion counter until it reaches \p serial, the budget runs out, or a completed
   * command buffer reports an execution error (see \ref lastErrorForTest). Completion handlers
   * run on a Metal-internal thread, so rechecking a counter is enough and keeps this backend free
   * of extra synchronization primitives.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds.
   */
  bool onWaitForSerial(uint64_t serial, double timeoutSeconds) override;
  Result<std::span<const uint8_t>> onMappedBytes(uint32_t mappingSlotIndex) const override;
  void onUnmapBuffer(uint32_t mappingSlotIndex) override;

private:
  /// Constructs an empty device; \ref Create attaches the Metal device.
  MetalDevice();

  struct Impl;  //!< Objective-C++ state (Metal objects and slot tables); defined in the .mm.
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::gpu::metal
