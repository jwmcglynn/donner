#pragma once
/// @file
/// \c donner::geode::GeodeWgpuAdapterDevice - the wgpu-backed \c donner::gpu::Device adapter.
///
/// TEMPORARY transition adapter. It is deleted per-platform as each native backend takes over
/// production rendering, and each escape hatch below is deleted with the change that migrates its
/// last caller.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeDevice;

/**
 * Implements \c donner::gpu::Device on top of the wgpu device a \ref GeodeDevice already owns,
 * so Geode subsystems can migrate onto the Donner GPU runtime one at a time while the
 * process still renders through wgpu underneath.
 *
 * Every `on*` hook receives input the base class already validated fail-closed, and translates
 * it to the corresponding webgpu.hpp call. wgpu objects are stored in per-kind slot vectors
 * indexed by the base class's slot indices; bind groups capture their wgpu layout object at
 * creation time (wgpu retains it internally), so encoding never resolves a layout by slot.
 *
 * Does NOT own the \ref GeodeDevice: the constructing scope must keep it alive for the
 * adapter's whole lifetime (in practice \ref GeodeDevice owns the adapter alongside its shared
 * pipelines).
 *
 * Thread affinity matches \c donner::gpu::Device: single-threaded use. Completion callbacks
 * touch only the atomic completed-serial counter.
 */
class GeodeWgpuAdapterDevice final : public gpu::Device {
public:
  /**
   * Constructs the adapter over \p geodeDevice.
   *
   * @param geodeDevice Device wrapper providing the wgpu device/queue; must outlive the adapter.
   */
  explicit GeodeWgpuAdapterDevice(GeodeDevice& geodeDevice);

  /// Destructor; waits for in-flight submissions (so deferred destructions drain), then releases
  /// every wgpu object the adapter still owns.
  ~GeodeWgpuAdapterDevice() override;

  /// Serial of the most recent submission whose queue work-done callback has fired (0 if none).
  /// wgpu delivers the callbacks during \ref waitForSerial's polling (and opportunistically on
  /// submit), so call \ref waitForSerial to guarantee progress.
  ///
  /// \warning The base class's `Device::poll()` does NOT drive wgpu polling - it only processes
  /// deferred destructions against the serial this method reports. Per-frame destroy+poll churn
  /// therefore defers unboundedly until something waits: a frame loop must call
  /// \ref waitForSerial on its frame cadence (or extend the adapter with a non-blocking wgpu
  /// poll) so completions are observed and deferred destroys drain.
  uint64_t completedSerial() const override;

  /**
   * Blocks until \ref completedSerial reaches \p serial or \p timeoutSeconds elapses, driving
   * `wgpu::Device::poll` so queued work-done callbacks are delivered (on Emscripten the poll
   * shim yields through Asyncify, mirroring \ref GeodeDevice's wait machinery). This is the
   * only entry point that drives wgpu polling for this adapter - see the warning on
   * \ref completedSerial.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Maximum time to wait, in seconds.
   * @return True once \ref completedSerial reached \p serial; false if the timeout (or the
   *   bounded poll-iteration budget) elapsed first.
   */
  bool waitForSerial(uint64_t serial, double timeoutSeconds);

  /**
   * TEMPORARY escape hatch (deleted with the readback and presentation migration): registers an
   * externally owned wgpu texture - e.g. a render target created by the host or an earlier
   * non-migrated subsystem - as a \c donner::gpu::Texture of this adapter so migrated code can
   * reference it in render passes and copies. The adapter does NOT take ownership; destroying
   * the returned handle only forgets the registration.
   *
   * @param texture Externally owned wgpu texture; must remain valid while registered.
   * @param size Texture extent in texels.
   * @param format Texel format matching the wgpu texture.
   * @param usage Usage flags matching the wgpu texture's capabilities.
   */
  /**
   * Destroys the backend object behind \p texture explicitly, then releases its slot.
   *
   * Releasing a texture handle on its own only drops this adapter's reference, which leaves the
   * backend object resident until the host runtime collects it; a succession of resized render
   * targets is exactly where that shows up as retained memory. Externally imported textures are
   * left alone: their owner is the embedder, not this adapter.
   *
   * @param texture Live texture handle of this adapter; consumed either way.
   */
  gpu::Status destroyTextureBacking(gpu::Texture&& texture);

  /**
   * Destroys the backend object behind \p buffer explicitly, then releases its slot.
   *
   * The buffer counterpart of \ref destroyTextureBacking, and needed for the same reason:
   * releasing a buffer handle on its own only drops this adapter's reference, which leaves the
   * backend object resident until the host runtime collects it. A readback buffer whose map was
   * cancelled, and a pooled readback set evicted to keep the pool inside its ceiling, both have
   * to release their memory at that moment rather than whenever a collector next runs.
   *
   * @param buffer Live buffer handle of this adapter; consumed either way.
   */
  gpu::Status destroyBufferBacking(gpu::Buffer&& buffer);

  /**
   * Whether any wait slice of \p mapping waited on the map's completion event rather than
   * polling for it.
   *
   * The distinction is a property of how this adapter waited, so it is reported here rather than
   * inferred by the caller; the readback statistics carry it because a browser frame that fell
   * back to polling takes orders of magnitude longer to observe the same completion.
   *
   * @param mapping Live mapping handle of this adapter.
   * @return True if a completion-event wait was used; false for a polled wait, an unknown
   *   handle, or a mapping no slice ever waited on.
   */
  bool mappingUsedTimedWaitAny(const gpu::BufferMapping& mapping) const;

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

  gpu::Result<gpu::Texture> importExternalTexture(wgpu::Texture texture, const gpu::Extent2d& size,
                                                  gpu::TextureFormat format,
                                                  gpu::TextureUsage usage);

  /**
   * Installs \p encoder as the host command encoder: while one is installed, a submitted
   * command stream is replayed into it instead of into an encoder this adapter owns, and this
   * adapter performs no queue submit of its own. One command buffer therefore still carries a
   * whole frame in recording order, including the spans a caller records directly into the same
   * encoder around the replayed ones.
   *
   * Temporary bridge: it exists because a frame encoder is shared with subsystems that have not
   * migrated onto the runtime yet, so their spans cannot be replayed through it. It is removed
   * once every subsystem sharing the frame encoder records through the runtime.
   *
   * The caller owns \p encoder, must keep it alive until it is replaced or cleared, and must
   * call \ref notifyHostSubmitted after each queue submit of a command buffer finished from it.
   * Until then the serials replayed into it are deliberately not observable as complete.
   *
   * @param encoder Host-owned command encoder to replay into.
   */
  void setHostCommandEncoder(wgpu::CommandEncoder encoder);

  /// Uninstalls the host command encoder, returning this adapter to owning and submitting the
  /// encoders it records into. Serials already replayed into a host encoder stay incomplete
  /// until \ref notifyHostSubmitted reports their submit.
  void clearHostCommandEncoder();

  /// True while a host command encoder is installed, i.e. while submissions are replayed into
  /// it instead of reaching the queue.
  ///
  /// This device is shared by everything drawing through one \ref GeodeDevice, but a host
  /// encoder belongs to the single caller that installed it. Anything else submitting during
  /// that window would be spliced into a command buffer it does not own, at a point in that
  /// buffer it cannot reason about, and its \ref submit would report success for work that has
  /// not reached the queue. A caller whose submission must stand on its own checks this first
  /// and declines.
  bool hasHostCommandEncoder() const;

  /**
   * Reports that the host submitted a command buffer carrying every stream replayed since the
   * previous report, so their completion can now be observed. Advances \ref completedSerial to
   * the highest replayed serial once the queue drains, exactly as an adapter-owned submit does.
   * A no-op when nothing has been replayed since the last report.
   */
  void notifyHostSubmitted();

  /**
   * Submits \p commands straight to the queue, even while a host command encoder is installed.
   *
   * \ref submit replays into that encoder so a frame stays one command buffer in recording
   * order, and a caller whose work must stand on its own is otherwise told to decline. The
   * snapshot readback cannot decline: it is asked for while another renderer's frame may be open,
   * and it has its own completion to wait on. Splicing it into that frame's buffer would leave it
   * unsubmitted until the frame ends, and the readback would wait out its whole deadline for a
   * map of work that had not reached the queue.
   *
   * Bypassing the frame is correct here by construction rather than by timing: the readback
   * reads textures whose contents were submitted before it was asked for, and writes only its own
   * staging texture and readback buffer, so it shares no resource with the spans the open frame
   * is still recording. A caller that cannot say the same must use \ref submit.
   *
   * Bridge machinery, removed with the host-encoder bridge itself.
   *
   * @param commands Finished command buffer; consumed.
   * @return Submission serial, or an error if encoding or submission failed.
   */
  gpu::Result<uint64_t> submitStandalone(gpu::CommandBuffer&& commands);

  /**
   * TEMPORARY escape hatch (deleted with the readback and presentation migration): returns the
   * wgpu texture behind \p texture, or a null handle if the handle does not name a live texture of
   * this adapter. Borrowed; the adapter (or the external owner) retains ownership.
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

  gpu::Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex,
                               gpu::MapMode mode, uint64_t offsetBytes,
                               uint64_t byteCount) override;
  gpu::MapSliceState onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) override;
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
                             const gpu::Extent2d& writeSize) override;
  gpu::Status onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                       std::span<const gpu::Command> commands) override;

private:
  /// One texture slot: the borrowed alias used for encoding, plus a +1 owning reference when
  /// the adapter created the texture (empty for imported external textures).
  struct TextureSlot {
    ScopedWgpuHandle<wgpu::Texture> ownedTexture;  //!< Owned +1 reference (adapter-created only).
    wgpu::Texture texture;                         //!< Borrowed alias; null when the slot is dead.
  };

  /// State shared with queue work-done callbacks.
  struct CompletionState {
    std::atomic<uint64_t> completedSerial{0};  //!< Highest completed submission serial.
  };

  /// Mutable state threaded through the encoding of one command stream.
  struct EncodingState {
    /// Owned encoder, used when no host encoder is installed; empty while replaying.
    ScopedWgpuHandle<wgpu::CommandEncoder> ownedEncoder;
    /// Encoder actually recorded into: \ref ownedEncoder, or the borrowed host encoder.
    wgpu::CommandEncoder encoder;
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

  /// Clears the slot of one non-pipeline resource kind, or returns false when the name is not
  /// one of them.
  /// @param resourceName Resource type name. @param slotIndex Slot to clear.
  bool clearResourceSlot(std::string_view resourceName, uint32_t slotIndex);

  /// Clears the slot of one pipeline-family resource kind, or returns false when the name is not
  /// one of them.
  /// @param resourceName Resource type name. @param slotIndex Slot to clear.
  bool clearPipelineSlot(std::string_view resourceName, uint32_t slotIndex);

  /// Attaches the queue work-done callback that advances \ref completedSerial to \p serial.
  /// @param serial Submission serial the callback completes.
  void advanceCompletedSerialWhenQueueDrains(uint64_t serial);

  GeodeDevice& geodeDevice_;

  /// Host-owned command encoder to replay into, or null when this adapter owns its encoders.
  wgpu::CommandEncoder hostCommandEncoder_;
  /// Set for the duration of one \ref submitStandalone so the submit below takes the
  /// adapter-owned encoder path even while a host encoder is installed.
  bool bypassHostEncoderForSubmit_ = false;

  /// Whether the next submit replays into the host encoder rather than reaching the queue: a
  /// host encoder is installed and this submit is not a standalone one.
  bool replaysIntoHostEncoder() const;
  /// Highest serial replayed into \ref hostCommandEncoder_ since the last
  /// \ref notifyHostSubmitted; 0 when nothing is awaiting the host's submit.
  uint64_t hostPendingSerial_ = 0;

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
    /// Whether any slice of this mapping waited on \ref mapFuture instead of polling. Reported
    /// out through \ref GeodeWgpuAdapterDevice::mappingUsedTimedWaitAny, because the readback
    /// stats distinguish the two and a browser frame that fell back to polling is a regression
    /// worth seeing.
    bool usedTimedWaitAny = false;
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

  /// Set only inside \ref importExternalTexture so \ref onCreateTexture registers the external
  /// texture instead of creating a new one.
  wgpu::Texture pendingImport_;
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
 * Maps wgpu texture usage flags onto the \c donner::gpu usage flags. Flags with no runtime
 * equivalent are dropped, so the result describes exactly the capabilities the runtime can
 * express for the texture.
 *
 * @param usage wgpu usage flags to map.
 */
gpu::TextureUsage GpuTextureUsageFromWgpu(wgpu::TextureUsage usage);

}  // namespace donner::geode
