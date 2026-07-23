#pragma once
/// @file
/// \c donner::geode::GeodeWgpuAdapterDevice - the wgpu-backed \c donner::gpu::Device adapter.
///
/// TEMPORARY design-0053 Phase 1 adapter. Removal gates: deleted per-platform at the Metal
/// cutover (change-seq 12), Linux/Windows Vulkan cutover (15), browser-bridge cutover (17); each
/// escape hatch is deleted with the packet that migrates its last caller (filters 10,
/// readback/presentation 11, ImGui 18; the 8a pipeline/bind-group-layout/sampler hatches were
/// deleted in packet 8b when GeoEncoder migrated onto gpu::CommandEncoder).

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeDevice;

/**
 * Implements \c donner::gpu::Device on top of the wgpu device a \ref GeodeDevice already owns,
 * so Geode subsystems can migrate onto the Donner GPU runtime one packet at a time while the
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
  /// wgpu delivers the callbacks while wgpu is polled - \ref pollCompletions (non-blocking) and
  /// \ref waitForSerial (blocking) both drive it.
  ///
  /// \warning The base class's `Device::poll()` does NOT drive wgpu polling - it only processes
  /// deferred destructions against the serial this method reports. The frame cadence MUST call
  /// \ref pollCompletions (or \ref waitForSerial) so completions are observed and deferred
  /// destroys drain; otherwise per-frame destroy churn defers unboundedly.
  uint64_t completedSerial() const override;

  /**
   * Non-blocking completion pump for the frame cadence: delivers any ready queue work-done
   * callbacks (advancing \ref completedSerial), then processes the base class's deferred
   * destructions against the new serial.
   *
   * On native this calls `wgpu::Device::poll(wait=false)`; on Emscripten the browser event loop
   * delivers the callbacks between frames, so only the base `Device::poll()` runs.
   *
   * On wasm/Emscripten, completions therefore only advance across event-loop yields: a
   * non-yielding multi-frame flow (e.g. a loop rendering many frames without returning to the
   * browser) never observes a new \ref completedSerial, so deferred destroys accumulate for the
   * duration of that flow. Such flows must yield to the event loop or call \ref waitForSerial
   * (which yields through Asyncify) to drain them.
   *
   * \warning Something on the frame cadence MUST call this (or \ref waitForSerial): the
   * deferred-destruction queue otherwise accumulates unboundedly (see the warning on
   * \ref completedSerial). `RendererGeode::endFrame` is the production caller.
   */
  void pollCompletions();

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
   * TEMPORARY escape hatch (deleted in packet 11, readback/presentation): registers an
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
  gpu::Result<gpu::Texture> importExternalTexture(wgpu::Texture texture, const gpu::Extent2d& size,
                                                  gpu::TextureFormat format,
                                                  gpu::TextureUsage usage);

  /**
   * TEMPORARY escape hatch (deleted in packet 11, readback/presentation): returns the wgpu
   * texture behind \p texture, or a null handle if the handle does not name a live texture of
   * this adapter. Borrowed; the adapter (or the external owner) retains ownership.
   *
   * @param texture Live texture handle of this adapter.
   */
  wgpu::Texture wgpuTextureOf(const gpu::Texture& texture) const;

  /**
   * TEMPORARY escape hatch (deleted in packet 11, readback/presentation): returns the wgpu
   * texture view behind \p textureView, or a null handle if unknown. Borrowed.
   *
   * @param textureView Live texture view handle of this adapter.
   */
  wgpu::TextureView wgpuTextureViewOf(const gpu::TextureView& textureView) const;

protected:
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

  GeodeDevice& geodeDevice_;

  std::vector<ScopedWgpuHandle<wgpu::Buffer>> slotBuffers_;
  std::vector<TextureSlot> slotTextures_;
  std::vector<ScopedWgpuHandle<wgpu::TextureView>> slotTextureViews_;
  std::vector<ScopedWgpuHandle<wgpu::Sampler>> slotSamplers_;
  std::vector<ScopedWgpuHandle<wgpu::BindGroupLayout>> slotBindGroupLayouts_;
  std::vector<ScopedWgpuHandle<wgpu::BindGroup>> slotBindGroups_;
  std::vector<ScopedWgpuHandle<wgpu::PipelineLayout>> slotPipelineLayouts_;
  std::vector<ScopedWgpuHandle<wgpu::ShaderModule>> slotShaderModules_;
  std::vector<ScopedWgpuHandle<wgpu::RenderPipeline>> slotRenderPipelines_;

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

}  // namespace donner::geode
