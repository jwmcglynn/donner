#pragma once
/// @file
/// \c donner::gpu::metal::MetalDevice - the Metal backend for the Donner GPU runtime.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
 * Scope: host-visible buffers and textures, MSL shader modules, render pipelines with a single
 * vertex buffer layout at slot 0 and bind group 0 only, render passes with color attachments,
 * compute pipelines and compute passes, and texture-to-buffer readback copies. Bindings follow
 * the deterministic argument-table mapping in
 * `donner/gpu/shader/MslBindingMap.h`: buffer binding `b` maps to Metal buffer index `1 + b`,
 * texture and sampler bindings map directly, and stage-in vertex data occupies vertex buffer
 * index 30.
 *
 * Queue writes update idle resources directly. Writes to resources an earlier submission still
 * uses are copied into bounded host storage and uploaded at the beginning of the next ordinary
 * submission. Repeated writes of the same resource range are coalesced. A batch adds one staging
 * allocation and no queue submission; Metal retains the staging buffer and destinations until
 * completion. Queued and in-flight uploads share a configurable byte budget, defaulting to
 * \ref kMaxBufferByteSize; at most 16,384 writes may await submission. Excess writes return
 * `LimitExceeded` without changing the batch. The budget counts logical upload bytes; packing
 * temporarily retains both the host payload and native staging buffer. Managed resources may
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
 * they touch only atomics and a mutex-protected error string, observable through
 * \ref completedSerial, \ref waitForSerial, and \ref lastErrorForTest.
 *
 * The header is pure C++ (Objective-C state lives behind a pimpl) so it is includable from C++
 * tests; the implementation is Objective-C++.
 */
class MetalDevice final : public Device {
public:
  /// Which memory model the backend builds its resources for.
  enum class MemoryModel : uint8_t {
    /// Take the model the Metal device reports. Production always uses this.
    Detected,
    /// Build for a device without unified memory whatever it reports, so the host-coherency
    /// steps that model needs are exercised on hardware that would otherwise never take them.
    ForceNonUnified,
  };

  /**
   * Creates a device on the system default Metal device. Returns nullptr if no Metal device is
   * available (for example on a CI host without a GPU).
   *
   * @param memoryModel Which memory model to build resources for; production leaves this
   *   detected, and a test forces the non-unified path to cover it on unified hardware.
   * @param uploadStagingByteBudget Maximum combined queued and in-flight upload staging bytes.
   *   Zero is invalid and returns nullptr. Each individual batch also fits \ref kMaxBufferByteSize.
   * @param unalignedWriteTimeout Maximum CPU wait for an unaligned write to a busy buffer.
   *   Must be between zero and five seconds; invalid budgets return nullptr.
   */
  static std::unique_ptr<MetalDevice> Create(
      MemoryModel memoryModel = MemoryModel::Detected,
      uint64_t uploadStagingByteBudget = kMaxBufferByteSize,
      std::chrono::milliseconds unalignedWriteTimeout = std::chrono::seconds(5));

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
    uint64_t inFlightStagingBytes = 0;    //!< Upload bytes reserved until GPU completion.
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

  /// Destructor; releases all Metal objects still alive.
  ~MetalDevice() override;

  /// Serial of the most recent submission whose Metal command buffer has completed on the GPU
  /// (0 if none). Updated by completion handlers, which may run on another thread.
  uint64_t completedSerial() const override;

  /**
   * Blocks until \ref completedSerial reaches \p serial or \p timeoutSeconds elapses, by polling
   * the completion counter (the completion handler runs on a Metal-internal thread, so a poll
   * loop with a short sleep is sufficient and keeps this backend free of extra sync primitives).
   *
   * Returns false on timeout, and also returns false if any completed command buffer reported an
   * execution error (see \ref lastErrorForTest).
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Maximum time to wait, in seconds.
   */
  bool waitForSerial(uint64_t serial, double timeoutSeconds);

  /**
   * Copies the full contents of \p buffer back to the host and returns the bytes.
   *
   * Test/readback convenience, pending a buffer mapping API: it validates device identity, slot
   * liveness, and the handle generation, then reads
   * the shared-storage Metal buffer contents directly. Callers must ensure relevant GPU work has
   * completed first (see \ref waitForSerial). Queued writes become visible after a subsequent
   * ordinary submission completes; this accessor does not submit them.
   *
   * @param buffer Buffer to read back; must be a live buffer of this device.
   */
  Result<std::vector<uint8_t>> readBackBuffer(const Buffer& buffer);

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
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override;
  Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                       std::span<const uint8_t> data) override;
  Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                        const TexelCopyBufferLayout& dataLayout,
                        const Extent2d& writeSize) override;
  Status onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                  std::span<const Command> commands) override;

private:
  /// Constructs an empty device; \ref Create attaches the Metal device.
  MetalDevice();

  struct Impl;  //!< Objective-C++ state (Metal objects and slot tables); defined in the .mm.
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::gpu::metal
