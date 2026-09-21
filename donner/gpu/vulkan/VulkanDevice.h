#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanDevice - the Vulkan backend for the Donner GPU runtime.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/gpu/Device.h"

namespace donner::gpu::vulkan {

struct VulkanApi;
struct VulkanSurfaceContext;
class VulkanSwapchain;

/// Selects presentation instance extensions from the names a loader offers.
/// Exposed for deterministic platform-companion extension tests.
/// @param offeredExtensions NUL-terminated names offered by the loader.
/// @param requiredExtensions NUL-terminated embedder-required names.
std::vector<const char*> SelectPresentationExtensionsForTest(
    std::span<const char* const> offeredExtensions,
    std::span<const char* const> requiredExtensions);

/**
 * Vulkan backend of the Donner GPU runtime.
 *
 * Inherits every fail-closed validation check from \ref donner::gpu::Device; the `on*` hooks
 * receive only validated input and translate it to Vulkan objects. Every Vulkan call's VkResult
 * is checked and any failure fails closed with a \ref donner::gpu::GpuError; the backend never
 * crashes on such failures.
 *
 * Scope is exactly the solid-fill vertical slice: buffers and 2D single-sample textures, SPIR-V
 * shader modules (from \ref donner::gpu::shader::EmitSpirv), the solid-fill render pipeline
 * family, render passes with color attachments, and texture-to-buffer readback copies. Bind
 * group index N maps directly to Vulkan descriptor set N; binding numbers map directly to
 * SPIR-V binding decorations (the SPIR-V emitter uses DescriptorSet 0 / Binding b).
 *
 * Targets Vulkan 1.1 core only: classic VkRenderPass + VkFramebuffer (no dynamic rendering),
 * per-submission VkFence completion tracking, and the core negative-viewport-height feature
 * (VK_KHR_maintenance1, promoted to 1.1) to present WebGPU clip-space semantics - identical
 * SPIR-V positions land on identical pixels as the wgpu baseline. Presentation additionally
 * requires swapchain maintenance1 and its matching instance extension dependencies. The headless
 * path needs no device extension; \ref CreateWithTimelineSemaphoreForTest optionally requests
 * VK_KHR_timeline_semaphore so a test can hold a submission open.
 *
 * Every buffer lives in HOST_VISIBLE | HOST_COHERENT memory and stays persistently mapped.
 * Idle queue writes copy directly. Four-byte-aligned writes to busy buffers copy their payload
 * into a bounded queue, flushed in order before the next ordinary submission, including empty
 * command streams. Unaligned writes wait up to five seconds for that buffer's prior submission;
 * timeout refuses the write without changing its bytes. Pending and in-flight write payloads
 * share a 1 GiB budget, and at most 16,384 distinct ranges may be pending. Identical ranges
 * coalesce at the newest write's position, preserving overlap order.
 *
 * Readback waits for the buffer's last use before reading mapped memory; a timeout or device
 * error returns an error without copying any bytes. Queued writes become visible after the
 * subsequent submission completes. Staging and destination allocations remain alive until its
 * fence signals, including destinations absent from the public command stream. Terminal queue
 * submission loss is latched before cleanup, and the device is drained before submitted objects
 * are freed. Vulkan requires this lost-device idle wait to return finitely, but the wait has no
 * caller-configured deadline. Later host accesses and submissions return the latched error.
 *
 * Which allocation a buffer is bound into is the allocator's decision, behind the seam in
 * VulkanBufferAllocator.h: one dedicated allocation per buffer today, with a suballocating
 * implementation replaceable there rather than at every call site, once measurement says the
 * driver's allocation-count cap is the constraint worth spending complexity on. Such a memory type
 * is guaranteed by the Vulkan specification ("Device Memory": at least one memory type has both
 * HOST_VISIBLE and HOST_COHERENT), and both the CI software rasterizer and desktop GPUs expose it.
 * Textures are DEVICE_LOCAL (when available) with staged uploads through a transient host-visible
 * buffer and explicit image layout transitions.
 *
 * Synchronization is tracked per image: barriers before and after render passes, dispatches,
 * and copies come from VulkanResourceState.h. Render-pass attachment layouts stay fixed, so
 * explicit image barriers provide the dependencies without varying render-pass compatibility.
 * Unknown usage falls back to an ALL_COMMANDS barrier. Readback copies also end in a HOST-domain
 * buffer barrier.
 *
 * Barrier elision - dropping a barrier the model says is needed - is still not attempted; that
 * would need counter and timing evidence naming the bottleneck it removes.
 *
 * Presentation is opt in through \ref CreateWithPresentationSupport, because a swapchain has to
 * be requested before any surface exists. Frames come from a VK_KHR_swapchain swapchain over a
 * surface the embedder's platform object names, or over a headless surface where there is no
 * window; a frame is acquired with a semaphore the first submission writing it waits on, and is
 * handed to the presentation engine by a barrier submitted after that work. A frame that is
 * discarded rather than presented cannot be given back, so the swapchain is recreated to reclaim
 * it. Everything above is inert on a device created by \ref Create, which refuses every surface.
 *
 * Threading: single-threaded use, matching \ref donner::gpu::Device's thread affinity.
 * Completion is tracked by polling per-submission fences from the owning thread; there are no
 * cross-thread callbacks. Creation is serialized against failed shutdown: if a bounded completion
 * wait cannot prove native work finished, the complete device graph is retained until process exit
 * and every later Vulkan device creation returns nullptr. Restart the process to retry Vulkan.
 *
 * The header is pure C++ (Vulkan state lives behind a pimpl) so it is includable without the
 * Vulkan headers on any platform; the implementation compiles against the hermetic
 * Vulkan-Headers module everywhere but can only link and execute where a Vulkan loader exists
 * (Linux in CI).
 */
class VulkanDevice final : public Device {
public:
  /// Native shader representation accepted by this device.
  ShaderSourceKind shaderSourceKind() const override { return ShaderSourceKind::Spirv; }

  /// True for Uint16 always; for Uint32 only when the physical device offered
  /// `fullDrawIndexUint32` and \ref Create enabled it, since without that feature index values
  /// above `maxDrawIndexedIndexValue` are undefined.
  bool supportsFullIndexRange(IndexFormat format) const override;

  /// Reports Uint32 as lacking its full range from now on, whatever the device enabled, so the
  /// refusal path runs on a device that has the feature. Only ever narrows the capability.
  void disableFullUint32IndexRangeForTest();

  /**
   * Creates a headless device: a VkInstance without surface extensions (enabling
   * VK_LAYER_KHRONOS_validation only when the loader enumerates it), the first physical device
   * exposing a graphics queue family, and a single VkDevice + VkQueue. Returns nullptr if no
   * Vulkan 1.1 instance or graphics-capable physical device is available.
   */
  static std::unique_ptr<VulkanDevice> Create();

  /// Creates a device with VK_KHR_timeline_semaphore enabled solely for test-owned host gates.
  /// Returns nullptr if the test extension/feature is unavailable; ordinary Create needs neither.
  static std::unique_ptr<VulkanDevice> CreateWithTimelineSemaphoreForTest();

  /**
   * Creates a device that can present: an instance with the surface extensions the loader offers
   * and a device with VK_KHR_swapchain enabled. Returns nullptr when the loader offers no surface
   * extension or the device no swapchain support.
   *
   * Presentation is opt in because a swapchain has to be asked for before any surface exists:
   * the device has to already carry swapchain support by the time `createSurface` is called, and
   * a device created by \ref Create refuses every surface rather than appearing to support one.
   * Whether the queue can present to a particular surface is checked when that surface is
   * created, since that is the first point at which the question can be asked.
   *
   * @param requiredInstanceExtensions Platform surface extensions the embedder needs enabled on
   *   the returned instance. The span and each non-null, NUL-terminated name it contains are
   *   borrowed synchronously and must remain readable until this call returns. Creation fails
   *   when the count does not fit Vulkan's uint32_t field or the loader does not offer a name.
   */
  static std::unique_ptr<VulkanDevice> CreateWithPresentationSupport(
      std::span<const char* const> requiredInstanceExtensions = {});

  /// Whether this device was created with presentation support. Test accessor, so a suite can
  /// say which device it is looking at rather than inferring it from a refusal.
  [[nodiscard]] bool supportsPresentation() const;

  /**
   * The Vulkan instance this device was created on, for an embedder that creates its own surface.
   *
   * A window-system surface is scoped to the instance it was created against, so an embedder
   * whose windowing library already knows how to make one (GLFW and SDL both do) needs this
   * instance to make it against; that is what keeps this build free of Xlib and Wayland headers.
   * Pass the resulting handle back as \ref donner::gpu::NativeSurfaceKind::EmbedderSurface.
   *
   * Returned as an opaque pointer so this header stays free of Vulkan types; it is a `VkInstance`.
   * This device owns it and it stays valid for the device's lifetime. Ownership of a surface made
   * against it stays with the embedder: destroy the runtime's surface first, then the embedder's,
   * then this device. Null when the device was created without presentation support, since
   * without the surface extensions there is nothing an embedder could create against it.
   */
  [[nodiscard]] void* nativeInstance() const;

  /// Destructor; waits for in-flight submissions (vkDeviceWaitIdle), drains deferred
  /// destructions, then destroys all remaining Vulkan objects in dependency-safe order.
  ~VulkanDevice() override;

  /// Serial of the most recent submission whose fence has signaled (0 if none). Polls pending
  /// fences with vkGetFenceStatus and advances a monotonic counter.
  uint64_t completedSerial() const override;

  /**
   * Copies the full contents of \p buffer back to the host and returns the bytes.
   *
   * Test/readback convenience for the vertical slice, pending the buffer mapping API: validates
   * the handle (null, device identity, and generation) through the base class, then reads the
   * persistently mapped host-visible allocation after waiting up to five seconds for this
   * buffer's last submission. Unrelated work does not delay an idle buffer. Timeout or device
   * failure returns an error without reading memory; fence completion makes device writes
   * visible to the host for HOST_COHERENT memory.
   *
   * @param buffer Buffer to read back; must be a live buffer of this device.
   */
  Result<std::vector<uint8_t>> readBackBuffer(const Buffer& buffer);

  /// The image layout a texture is tracked in, as the backend's own bookkeeping records it.
  ///
  /// Reported through a backend-neutral enum so the header stays free of Vulkan types. This is a
  /// test accessor: a texture the backend never transitioned to the layout its descriptors
  /// declare is invalid use that a permissive driver executes anyway, so the mistake is not
  /// observable in the pixels a slice reads back.
  enum class TrackedTextureLayout : uint8_t {
    Undefined,        //!< Never transitioned; contents undefined.
    General,          //!< Readable and writable, the layout a storage-texture binding declares.
    ShaderReadOnly,   //!< The layout a sampled-texture binding declares.
    TransferSrc,      //!< Source of a copy.
    TransferDst,      //!< Destination of a copy or upload.
    ColorAttachment,  //!< Bound as a render pass color attachment.
    Other,            //!< Any layout this enum does not name.
  };

  /**
   * Returns the layout \p texture is tracked in. Test accessor; fails closed on a handle that
   * does not name a live texture of this device.
   *
   * @param texture Texture to query.
   */
  Result<TrackedTextureLayout> trackedTextureLayoutForTest(const Texture& texture) const;

  /// One image barrier the backend recorded, reported as plain numbers so this header stays free
  /// of Vulkan types. Test accessor: which barriers are emitted is the whole contract of the
  /// resource-state model, and it is not observable in the pixels a slice reads back.
  struct RecordedImageBarrierForTest {
    uint32_t textureSlot = 0;  //!< Slot of the texture the barrier applies to.
    uint32_t srcStage = 0;     //!< Source pipeline stage mask.
    uint32_t dstStage = 0;     //!< Destination pipeline stage mask.
    uint32_t srcAccess = 0;    //!< Access made available.
    uint32_t dstAccess = 0;    //!< Access made visible.
    int32_t oldLayout = 0;     //!< Layout the image was in.
    int32_t newLayout = 0;     //!< Layout the image moved to.
  };

  /// Enables an empty barrier history or disables recording and releases the history.
  /// Recording is disabled by default.
  /// @param enabled Whether subsequent image barriers should be retained for inspection.
  void setImageBarrierRecordingForTest(bool enabled);

  /// Image barriers since recording was last enabled, oldest first; empty while disabled.
  [[nodiscard]] std::vector<RecordedImageBarrierForTest> recordedImageBarriersForTest() const;

  /// Where an injected upload failure happens, which is what decides whether the transitions it
  /// recorded describe anything the GPU will run.
  enum class UploadFailureModeForTest : uint8_t {
    /// The upload fails before anything reaches the queue, so its recorded work never runs.
    BeforeSubmit,
    /// The submission reaches the queue and the wait for it reports a timeout, so its recorded
    /// work is still pending and will run.
    AfterSubmit,
  };

  /// Makes the next internal texture upload report failure at \p mode, so a test can prove what
  /// the tracker is left holding in each case. Test seam; the upload's Vulkan work is recorded
  /// either way, only its reported result is replaced.
  ///
  /// @param mode Where the injected failure happens.
  void failNextTextureUploadForTest(UploadFailureModeForTest mode);

  /// Returns the number of timed-out uploads still owned by the device, without polling.
  size_t pendingTextureUploadCountForTest() const;

  /// Defers upload fence polling to exercise ownership before device teardown.
  /// @param defer Whether to postpone upload completion observations.
  void deferTextureUploadPollingForTest(bool defer);

  /// Borrowed native objects for tests that submit their own synchronization gate. Valid only
  /// until this device is destroyed; callers must use the owning thread, release every gate, and
  /// finish their native work before destroying any of these objects or the device.
  struct NativeContextForTest {
    const VulkanApi* api;       //!< Device entry points, owned by this device.
    void* instance;             //!< Borrowed VkInstance, for a test that stands in for an embedder.
    void* device;               //!< Borrowed VkDevice.
    void* queue;                //!< Borrowed VkQueue.
    uint32_t queueFamilyIndex;  //!< Family of the borrowed queue.
  };

  /// Returns borrowed native objects solely for deterministic backend synchronization tests.
  NativeContextForTest nativeContextForTest() const;

  /// Builds an otherwise-empty device owner around fake native handles for teardown tests.
  /// @param api Copied native callbacks. @param instanceHandle Fake instance.
  /// @param deviceHandle Fake device. @param commandPoolHandle Fake command pool.
  /// @param onAdmission Optional callback invoked while holding the creation gate.
  /// @param admissionContext Opaque argument for the admission callback.
  static std::unique_ptr<VulkanDevice> CreateForTeardownTest(const VulkanApi* api,
                                                             uint64_t instanceHandle,
                                                             uint64_t deviceHandle,
                                                             uint64_t commandPoolHandle,
                                                             void (*onAdmission)(void*) = nullptr,
                                                             void* admissionContext = nullptr);

  /// Exercises native instance/device creation through a fake API and immediately destroys them.
  /// @param api Fake Vulkan callbacks. @param enablePresentation Whether to enable presentation.
  /// @param requiredInstanceExtensions Embedder-required extension names.
  static bool CreateNativeObjectsForPresentationTest(
      const VulkanApi& api, bool enablePresentation,
      std::span<const char* const> requiredInstanceExtensions = {});

  /// Adds fake pending native work to the real ownership graph, without submitting to a GPU.
  /// @param upload Whether this is an internal texture upload instead of an ordinary submission.
  /// @param fenceHandle Nonzero fake fence. @param commandBufferHandle Nonzero fake command buffer.
  void attachWorkForTeardownTest(bool upload, uint64_t fenceHandle, uint64_t commandBufferHandle);

  /// Returns the fake device's borrowed native context and shared child lifetime token.
  VulkanSurfaceContext surfaceContextForTeardownTest() const;

  /// Attaches a real swapchain owner to the fake device teardown graph.
  void attachSurfaceForTeardownTest(std::unique_ptr<VulkanSwapchain> surface);

  /// Snapshot of queued buffer-write ownership, without polling the queue.
  struct BufferWriteStats {
    size_t pendingWrites = 0;         //!< Coalesced writes awaiting an ordinary submission.
    uint64_t pendingBytes = 0;        //!< Owned payload bytes awaiting submission.
    uint64_t inFlightBytes = 0;       //!< Packed staging bytes retained until fence completion.
    uint64_t stagingAllocations = 0;  //!< Successful staging allocations, including failed submits.
    uint64_t submittedBatches = 0;    //!< Accepted submissions containing queued writes.
    size_t retainedDestinations = 0;  //!< Retired destinations retained by outstanding uploads.
    uint64_t lostDeviceDrains = 0;    //!< Lost-device submission drains completed before cleanup.
  };

  /// Returns the current queued-write counters without waiting or changing state.
  BufferWriteStats bufferWriteStatsForTest() const;

  /// Lowers the combined pending/in-flight payload limit for deterministic budget tests.
  /// @param byteBudget Maximum payload bytes, clamped to the production limit.
  void setBufferWriteByteBudgetForTest(uint64_t byteBudget);

  /// Makes the next native submission fail before it reaches the queue, after encoding finishes.
  /// @param deviceLost Whether to inject terminal device loss instead of recoverable host OOM.
  void failNextSubmissionForTest(bool deviceLost = false);

  /// Makes the next acquisition on the surface at \p surfaceSlotIndex report the swapchain as
  /// out of date, so its rebuild-and-retry path runs. Test seam; see the swapchain's own note for
  /// why a headless surface needs one.
  /// @param surfaceSlotIndex Slot of a live surface of this device.
  void forceNextAcquireOutOfDateForTest(uint32_t surfaceSlotIndex);

  /// Makes the next swapchain creation for the surface at \p surfaceSlotIndex ask for the fewest
  /// images allowed, so a rebuild shrinks its acquisition ring. Test seam.
  /// @param surfaceSlotIndex Slot of a live surface of this device.
  void forceMinimumImageCountOnceForTest(uint32_t surfaceSlotIndex);

  /// What a surface's acquisition bookkeeping looks like from outside, for the contracts whose
  /// only other symptom is timing dependent.
  struct SurfaceAcquisitionForTest {
    bool live = false;             //!< Whether that slot holds a surface at all.
    bool owesAcquireWait = false;  //!< Whether it still owes its frame's acquisition wait.
    size_t frameRingSlot = 0;      //!< Ring slot the frame it holds was acquired on.
    std::optional<size_t> lastFencedRingSlot;  //!< Ring slot its last handover fenced.
    size_t ringSize = 0;                       //!< Slots in its acquisition ring.
  };

  /// Reads the acquisition bookkeeping of the surface at \p surfaceSlotIndex. Test accessor.
  /// @param surfaceSlotIndex Slot of a surface of this device.
  [[nodiscard]] SurfaceAcquisitionForTest surfaceAcquisitionForTest(
      uint32_t surfaceSlotIndex) const;

  /// First latched Vulkan failure observed during submission, polling, or waiting on fences
  /// (e.g. VK_ERROR_DEVICE_LOST), or an empty string if none occurred.
  /// Test/diagnostic accessor.
  std::string lastErrorForTest() const;

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

  // Host buffer mapping. Every buffer this backend allocates is already host-visible, coherent,
  // and persistently mapped, so a mapping is the wait for the work that fills it plus a
  // bounds-checked view of that existing pointer; no second vkMapMemory is taken, because a
  // memory object may only be mapped once at a time.
  Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex, MapMode mode,
                          uint64_t offsetBytes, uint64_t byteCount) override;
  MapSliceReport onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) override;

  /**
   * Blocks on the pending submission's fence with `vkWaitForFences`, so the wait is woken by the
   * submission completing rather than by rechecking. Returns false on timeout, if the serial was
   * never submitted, or if a Vulkan call fails (see \ref lastErrorForTest).
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds.
   */
  bool onWaitForSerial(uint64_t serial, double timeoutSeconds) override;
  Result<std::span<const uint8_t>> onMappedBytes(uint32_t mappingSlotIndex) const override;
  void onUnmapBuffer(uint32_t mappingSlotIndex) override;

private:
  friend class VulkanSwapchainTestAccess;

  /// Waits for a buffer's last use and reports timeout or device error before host access.
  /// @param serial Last submitted use of this buffer.
  /// @param operation Operation name included in a timeout diagnostic.
  Status waitForBufferAccess(uint64_t serial, std::string_view operation);

  /// Creates the common backend, optionally enabling the extension used by native test gates and
  /// the surface/swapchain extensions presentation needs.
  /// @param enableTimelineSemaphoreForTest Whether to request VK_KHR_timeline_semaphore.
  /// @param enablePresentation Whether to request the surface and swapchain extensions.
  static std::unique_ptr<VulkanDevice> CreateImpl(
      bool enableTimelineSemaphoreForTest, bool enablePresentation,
      std::span<const char* const> requiredInstanceExtensions = {});

  /// Constructs an empty device; \ref Create attaches the Vulkan instance/device.
  VulkanDevice();

  struct Impl;  //!< Vulkan state (handles and slot tables); defined in the .cc.
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::gpu::vulkan
