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
#include <functional>
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
 * touch only shared completion state, guarded independently of the adapter lifetime.
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
  /// wgpu delivers the callbacks during \ref gpu::Device::waitForSerial's polling (and
  /// opportunistically on submit), so call it to guarantee progress.
  ///
  /// \warning The base class's `Device::poll()` does NOT drive wgpu polling - it only processes
  /// deferred destructions against the serial this method reports. Per-frame destroy+poll churn
  /// therefore defers unboundedly until something waits: a frame loop must call
  /// \ref gpu::Device::waitForSerial on its frame cadence (or extend the adapter with a
  /// non-blocking wgpu poll) so completions are observed and deferred destroys drain.
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
   * TEMPORARY escape hatch (deleted with the readback and presentation migration): registers an
   * externally owned wgpu texture - e.g. a render target created by the host or an earlier
   * non-migrated subsystem - as a \c donner::gpu::Texture of this adapter so migrated code can
   * reference it in render passes and copies. The adapter does NOT take ownership; destroying
   * the returned handle only forgets the registration, and
   * \ref gpu::Device::ownsTextureBacking reports false for it.
   *
   * @param texture Externally owned wgpu texture; must remain valid while registered.
   * @param size Texture extent in texels.
   * @param format Texel format matching the wgpu texture.
   * @param usage Usage flags matching the wgpu texture's capabilities.
   */
  gpu::Result<gpu::Texture> importExternalTexture(wgpu::Texture texture, const gpu::Extent2d& size,
                                                  gpu::TextureFormat format,
                                                  gpu::TextureUsage usage);

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

  /// Destroys the wgpu buffer in \p slotIndex, so the allocation goes back now rather than when
  /// the host runtime next collects. @param slotIndex Validated live buffer slot.
  void onDestroyBufferBacking(uint32_t slotIndex) override;

  /// Destroys the wgpu texture in \p slotIndex if this adapter allocated it; an external
  /// registration belongs to the embedder and is left alone.
  /// @param slotIndex Validated live texture slot.
  void onDestroyTextureBacking(uint32_t slotIndex) override;

  /// Whether \p slotIndex holds a texture this adapter allocated, rather than one registered
  /// through \ref importExternalTexture. @param slotIndex Validated live texture slot.
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
    ScopedWgpuHandle<wgpu::CommandEncoder> ownedEncoder;  //!< Encoder this adapter owns.
    wgpu::CommandEncoder encoder;  //!< Borrowed alias of \ref ownedEncoder, used for encoding.
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

  GeodeDevice& geodeDevice_;

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
