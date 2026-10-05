#pragma once
/// @file
/// Test-only wgpu-native implementation of \c donner::gpu::Device, the reference the Linux resvg
/// comparison renders the production Geode contexts through.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/base/SmallVector.h"
#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeDevice;
class GeodeGpuRoot;

/// The wgpu objects one reference device is reached through, released together.
struct GeodeWgpuRoots {
  wgpu::Instance instance;  //!< Instance the adapter came from.
  wgpu::Adapter adapter;    //!< Adapter the device came from.
  wgpu::Device device;      //!< Device every runtime device over this root records against.
  wgpu::Queue queue;        //!< Default queue of \ref device.

  /// Token the device-lost callback this process installed retains, or null when it installed
  /// none. Released with the handles.
  void* deviceLostCallbackToken = nullptr;
};

/**
 * The wgpu objects every reference runtime device records against, and the sticky loss condition
 * they share. Retained through `shared_ptr` by each runtime device over it, so the handles outlive
 * the last of them.
 *
 * Produced only by \ref SelectWgpuReference, so a root that exists is one whose device and queue
 * are complete.
 */
class WgpuReferenceRoot {
public:
  /**
   * Retains one complete set of wgpu objects.
   *
   * @param handles Objects the selection produced; released with this root.
   * @param lostState Sticky loss condition shared by every runtime device over these objects.
   */
  WgpuReferenceRoot(GeodeWgpuRoots handles, std::shared_ptr<gpu::DeviceLostState> lostState);

  /// Releases the wgpu handles. A lost root retains its driver handles rather than risking a
  /// call into a driver that stopped answering.
  ~WgpuReferenceRoot();

  WgpuReferenceRoot(const WgpuReferenceRoot&) = delete;
  WgpuReferenceRoot& operator=(const WgpuReferenceRoot&) = delete;

  /// Instance the adapter came from. Borrowed; the root retains it.
  const wgpu::Instance& instance() const UTILS_LIFETIME_BOUND { return handles_.instance; }
  /// Adapter the device came from. Borrowed; the root retains it.
  const wgpu::Adapter& adapter() const UTILS_LIFETIME_BOUND { return handles_.adapter; }
  /// Device every runtime device over this root records against. Borrowed.
  const wgpu::Device& device() const UTILS_LIFETIME_BOUND { return handles_.device; }
  /// Default queue of \ref device. Borrowed.
  const wgpu::Queue& queue() const UTILS_LIFETIME_BOUND { return handles_.queue; }
  /// Sticky loss condition shared by every runtime device over this root.
  const std::shared_ptr<gpu::DeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return lostState_;
  }

private:
  GeodeWgpuRoots handles_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
};

/// A selected wgpu reference: the Geode root production contexts are created over, and the wgpu
/// objects behind it.
struct WgpuReferenceSelection {
  /// Root of kind \ref donner::geode::GpuBackendKind::External "GpuBackendKind::External" whose
  /// runtime devices are wgpu reference devices.
  std::shared_ptr<GeodeGpuRoot> root;
  /// The wgpu objects every runtime device over \ref root records against.
  std::shared_ptr<const WgpuReferenceRoot> reference;
};

/**
 * Selects the wgpu-native reference: creates an instance, requests an adapter and a device, and
 * takes the default queue, then adopts them as a Geode root whose runtime devices are
 * \ref donner::geode::GeodeWgpuAdapterDevice "GeodeWgpuAdapterDevice" instances.
 *
 * `WGPU_BACKEND` names the wgpu backend (`vulkan`, `metal`, `opengl` or `opengles`; Vulkan when
 * unset on Linux), and `DONNER_GEODE_FORCE_FALLBACK_ADAPTER=1` requests wgpu's fallback adapter.
 * Adapter and device requests that fail under parallel load are retried on a short logged
 * schedule.
 *
 * The production contexts, renderer and caches drive the reference only through the runtime
 * contract, so a comparison against it exercises the same Geode code as a native backend.
 *
 * @param label Label the selected device carries in driver diagnostics.
 * @return The selection, with both members null when no adapter or device could be obtained.
 */
WgpuReferenceSelection SelectWgpuReference(std::string_view label);

/**
 * Creates a headless Geode context over a newly selected wgpu reference.
 *
 * @param textureFormat Format the context's render targets and pipelines are built for.
 * @return The context, or null when \ref donner::geode::SelectWgpuReference "SelectWgpuReference"
 *   found no adapter or device.
 */
std::unique_ptr<GeodeDevice> CreateWgpuReferenceContext(
    gpu::TextureFormat textureFormat = gpu::TextureFormat::RGBA8Unorm);

/**
 * Implements \c donner::gpu::Device over one selected wgpu device, so the resvg comparison drives
 * the production Geode contexts and renderer through an independent WebGPU implementation.
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
   * Constructs a runtime device over \p root. Contexts receive them from the root
   * \ref SelectWgpuReference adopts.
   *
   * @param root wgpu objects to render through; must not be null.
   */
  explicit GeodeWgpuAdapterDevice(std::shared_ptr<const WgpuReferenceRoot> root);

  /// The wgpu objects this device renders through. Borrowed; the device retains them.
  const WgpuReferenceRoot& root() const UTILS_LIFETIME_BOUND { return *root_; }

  /// Destructor; waits for in-flight submissions (so deferred destructions drain), then releases
  /// every wgpu object the adapter still owns.
  ~GeodeWgpuAdapterDevice() override;

  /// Serial of the most recent submission whose queue work-done callback has fired (0 if none).
  /// wgpu delivers callbacks during \ref gpu::Device::poll or a bounded serial wait (and
  /// opportunistically on submit).
  ///
  /// \ref gpu::Device::poll drives one nonblocking wgpu poll when destruction is pending, then
  /// reclaims resources whose completion callback has arrived. An idle owner can call it without
  /// another submission or a serial wait.
  /// Capped by \ref holdSubmittedWorkForTesting while a test holds submitted work incomplete.
  uint64_t completedSerial() const override;

  /**
   * Makes this device behave like one that accepted work and stopped retiring it.
   *
   * Test seam: a healthy device completes everything in microseconds, so the bounded waits and
   * their deadlines are otherwise unreachable. Lifting the ceiling, from any thread, delivers the
   * held completions the way a callback does, waking a wait that is waiting for them.
   *
   * @param completedSerialCeiling Highest serial \ref completedSerial may report;
   *   \ref kNoCompletedSerialCeiling restores what the backend reports.
   */
  void holdSubmittedWorkForTesting(uint64_t completedSerialCeiling);

  /// Ceiling value that leaves \ref completedSerial reporting what the backend reports.
  static constexpr uint64_t kNoCompletedSerialCeiling = std::numeric_limits<uint64_t>::max();

protected:
  void onPollBackend() override;

  gpu::Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex,
                               gpu::MapMode mode, uint64_t offsetBytes,
                               uint64_t byteCount) override;
  gpu::MapSliceReport onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) override;

  /**
   * Polls the backend without blocking until \ref completedSerial reaches \p serial, the device
   * is lost, or the budget elapses (see `waitForSerialBounded`). Idle retirement instead uses a
   * single nonblocking \ref gpu::Device::poll iteration.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds.
   */
  bool onWaitForSerial(uint64_t serial, double timeoutSeconds) override;

private:
  /// Longest a serial wait rests between nonblocking polls before checking again for a loss,
  /// which is not signalled, and polling again in case nothing else is driving the queue. A
  /// delivered completion wakes the wait at once.
  static constexpr std::chrono::milliseconds kSerialWaitSlice{1};

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

  /// Polls the backend without blocking, resting up to \ref kSerialWaitSlice between polls for a
  /// delivered completion, until \ref completedSerial reaches \p serial, the device is lost, or
  /// the budget elapses. No poll blocks, so the wait always ends at its deadline even when a
  /// submission has stalled in the driver.
  ///
  /// @param serial Submission serial to wait for.
  /// @param timeoutSeconds Longest to wait, in seconds.
  /// @param onTimeout What a wait that spends its whole budget publishes.
  /// @return True once this device has completed \p serial.
  bool waitForSerialBounded(uint64_t serial, double timeoutSeconds, LossOnTimeout onTimeout);

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

  /// Highest serial \ref completedSerial may report; see \ref holdSubmittedWorkForTesting.
  std::atomic<uint64_t> completedSerialCeiling_{kNoCompletedSerialCeiling};

  /// Budget \ref ~GeodeWgpuAdapterDevice spends draining submitted work, in seconds. Generous: a
  /// healthy device drains in microseconds, so it only trips on a driver that has effectively
  /// hung, and teardown proceeds either way.
  static constexpr double kTeardownDrainSeconds = 5.0;

protected:
  /// Destroys the wgpu buffer in \p slotIndex, so the allocation goes back now rather than when
  /// the host runtime next collects. @param slotIndex Validated live buffer slot.
  void onDestroyBufferBacking(uint32_t slotIndex) override;

  /// Destroys the wgpu texture in \p slotIndex if this adapter allocated it; a registration of a
  /// sibling adapter's export belongs to that sibling and is left alone.
  /// @param slotIndex Validated live texture slot.
  void onDestroyTextureBacking(uint32_t slotIndex) override;

  /// Whether \p slotIndex holds a texture this adapter allocated, rather than a sibling's export
  /// named through \ref onRegisterTexture.
  /// @param slotIndex Validated live texture slot.
  [[nodiscard]] bool onOwnsTextureBacking(uint32_t slotIndex) const override;

  /// Identifies the wgpu device this adapter records against.
  gpu::BackendDeviceIdentity backendDeviceIdentity() const override;

  /**
   * Exports the texture in \p slotIndex, taking a reference on the wgpu texture and on the root
   * so the texture outlives this adapter if a registration does. Every adapter over one root
   * submits to the root's one queue, so registrations are ordered by submission order alone.
   *
   * @param slotIndex Validated live texture slot.
   */
  gpu::Result<gpu::BackendTextureExport> onExportTexture(uint32_t slotIndex) override;

  /**
   * Names a sibling adapter's exported texture in \p slotIndex, refusing one whose adapter
   * submits to a different queue. Nothing is allocated or counted.
   *
   * @param slotIndex Slot the registration occupies.
   * @param backing The producer's export.
   */
  gpu::Status onRegisterTexture(uint32_t slotIndex,
                                const gpu::ExportedTextureBacking& backing) override;
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
  /// The bytes \ref onWriteTexture handed the queue for the write it just accepted: the caller's
  /// span, or the repacked rows when the span ends at a minimal final row.
  uint64_t onTextureWriteByteCount(std::span<const uint8_t> data) const override;
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
    /// Completes exactly one queued range, publishes the contiguous completed prefix and wakes
    /// the waits for it.
    /// @param ticket Unique first serial of the completed range.
    void complete(uint64_t ticket);
    /// Wakes every wait for a delivered completion, after whatever it reads has been published.
    void notifyProgress();

    std::atomic<uint64_t> completedSerial{0};  //!< Highest serial with no unfinished predecessor.
    std::mutex mutex;  //!< Protects ranges and their completed high-water mark.

    /// Signalled with \ref mutex whenever a completion is delivered.
    std::condition_variable progressed;
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
  std::shared_ptr<const WgpuReferenceRoot> root_;

  /// Bytes the most recent accepted \ref onWriteTexture handed the queue; see
  /// \ref onTextureWriteByteCount.
  uint64_t lastTextureUploadBytes_ = 0;

  /// State of one pending or completed host mapping.
  ///
  /// The completion flag is shared with the backend's callback, which may fire from inside any
  /// call into the backend, so it is atomic and outlives this record through a reference count
  /// the callback also holds.
  struct MappingSlot {
    /// Shared completion state for an asynchronous buffer mapping.
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

    /// Future the map request returned; its id tells this request apart from a later one that
    /// reused the slot while a wait slice was polling.
    wgpu::Future mapFuture{};
  };

  /// The mapping's state right now: Ready or Failed once the map has completed, DeviceLost on a
  /// lost device, and Pending until then.
  /// @param completion Completion state to read.
  gpu::MapSliceState sliceStateOf(const MappingSlot::Completion& completion) const;

  /// Revalidates a slot after a poll that may have run callbacks or reentrant slot changes.
  bool mappingStillMatches(uint32_t mappingSlotIndex, const MappingSlot::Completion* completion,
                           wgpu::Future future) const;

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

}  // namespace donner::geode
