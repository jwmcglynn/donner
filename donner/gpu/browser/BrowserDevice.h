#pragma once
/// @file
/// \c donner::gpu::browser::BrowserDevice - the browser backend of the Donner GPU runtime.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/browser/BrowserBridge.h"
#include "donner/gpu/browser/BrowserObjectTable.h"

namespace donner::gpu::browser {

class BrowserDevice;

/**
 * A request for a browser GPU device, in progress.
 *
 * A browser hands over a device through promises it settles on its own schedule, so acquiring one
 * is not something a constructor can report. The request is begun, polled until it settles, and
 * then either yields a device or says why it will not. Nothing may be built on the bridge while
 * the request is still pending.
 */
class BrowserDeviceRequest {
public:
  /**
   * Asks the browser for a device over \p bridge and returns the request.
   *
   * A bridge that refuses to begin - a context that does not own the device, or one with no GPU
   * service reachable at all - produces a request that is already failed, with \ref error saying
   * why. Reporting it that way rather than as a separate failure channel means a caller writes
   * one settled-state check instead of two, and cannot handle the early refusal differently from
   * the late one by accident.
   *
   * @param bridge Bridge to the browser's GPU service; the request takes ownership.
   */
  static BrowserDeviceRequest Begin(std::unique_ptr<BrowserBridge> bridge);

  /// Move constructor. @param other Request to move from.
  BrowserDeviceRequest(BrowserDeviceRequest&& other) noexcept;

  /// Move assignment. @param other Request to move from.
  BrowserDeviceRequest& operator=(BrowserDeviceRequest&& other) noexcept;

  BrowserDeviceRequest(const BrowserDeviceRequest&) = delete;
  BrowserDeviceRequest& operator=(const BrowserDeviceRequest&) = delete;

  /// Destructor; abandons the request and releases the bridge if the device was never taken.
  ~BrowserDeviceRequest();

  /// How far the request has progressed. Polled; the browser settles it asynchronously.
  BrowserDeviceRequestState state() const;

  /// Why the request failed, when \ref state reports a failure. Empty otherwise.
  RcString error() const;

  /**
   * Takes the device, consuming the request.
   *
   * Fails closed unless \ref state reports \ref BrowserDeviceRequestState::Ready, so a caller
   * that skipped the wait gets a named error instead of a device that does not exist yet.
   */
  Result<std::unique_ptr<BrowserDevice>> take() &&;

private:
  /// Constructs a request over \p bridge. @param bridge Bridge the request was begun on.
  /// @param beginError Why the request could not be begun at all; empty when it was.
  BrowserDeviceRequest(std::unique_ptr<BrowserBridge> bridge, RcString beginError);

  std::unique_ptr<BrowserBridge> bridge_;

  /// Why \ref Begin could not start the request; empty when it did. A request that never reached
  /// the browser reports itself failed through this rather than by polling a bridge that was
  /// never asked anything.
  RcString beginError_;
};

/**
 * Browser backend of the Donner GPU runtime.
 *
 * Inherits every fail-closed validation check from \ref donner::gpu::Device; the `on*` hooks
 * receive only validated input and express it to the browser's GPU service through a
 * \ref BrowserBridge. Browser objects are named across that boundary by \ref BrowserObjectId
 * rather than by slot index, so an identifier retained past its object's destruction names
 * nothing instead of naming whatever the runtime put in that slot next.
 *
 * Three refusals are this backend's own, on top of the base class's:
 *
 * - An operation naming a resource this device has no browser object for fails with
 *   \ref GpuErrorType::InvalidHandle. That is a defense in depth rather than the primary check:
 *   the base class resolves the handle first, so reaching here with an unregistered slot means
 *   the two records disagree, which is exactly the condition worth failing on.
 * - An operation issued from a context that does not own the browser device fails with
 *   \ref GpuErrorType::InvalidState. A browser device belongs to the worker that obtained it and
 *   its objects are unusable elsewhere, so this is a hard boundary, not a convention.
 * - Every operation that can be refused fails with \ref GpuErrorType::InvalidState once the
 *   browser reports the device lost. Loss is permanent: a lost device is never regained, and a
 *   pending mapping on one can never complete, so waits end immediately rather than spending their
 *   budget. Release and teardown paths deliberately proceed instead, so a lost device still frees
 *   the browser objects it holds; refusing those would strand them for the life of the page.
 *
 * Presentation follows the browser's model rather than the runtime's default one: a browser
 * decides for itself when a canvas is shown, so \ref Device::presentSurface is refused on this
 * backend and a frame ends by abandoning its acquired texture. Acquiring, configuring and
 * reconfiguring behave as the runtime documents.
 *
 * What a \ref NativeSurfaceKind::CanvasSelector names is narrower here than the descriptor
 * suggests, and narrower in a worker than on the main thread. On the main thread it is a CSS
 * selector, resolved against the document. A worker has no document, and a canvas reaches one by
 * being transferred, so there it names the canvas by its element id and a selector that is not a
 * plain id will not resolve. The editor renders from a worker, so that is the case to write
 * against; passing `"#canvas"` satisfies both readings, which is why it is the form used
 * throughout. Widening this back out is a change to the descriptor's own documentation and belongs
 * with the unit that owns it.
 *
 * The compiled WGSL projection is what this backend accepts, matching the browser's own shading
 * language.
 *
 * Thread affinity: single-threaded use, matching \ref donner::gpu::Device, and additionally
 * pinned to the context that obtained the browser device.
 */
class BrowserDevice final : public Device {
public:
  /// Destructor; releases every browser object this device still owns.
  ~BrowserDevice() override;

  /// Shader representation this device accepts; the browser's own shading language.
  ShaderSourceKind shaderSourceKind() const override { return ShaderSourceKind::Wgsl; }

  /// Serial of the most recent submission the browser has reported finished (0 if none).
  uint64_t completedSerial() const override;

  /// Whether the browser has reported the device lost. Once true it never becomes false again.
  bool isDeviceLost() const;

  /// What the browser said when it reported the device lost. Empty while the device is alive.
  RcString deviceLostReason() const;

  /// The bridge this device speaks to the browser through. Test accessor.
  BrowserBridge& bridgeForTest() { return *bridge_; }

  /// Number of browser objects this device currently owns. Test accessor, for the teardown and
  /// slot-reuse contracts.
  size_t liveObjectCountForTest() const { return objects_.liveCount(); }

  /// How many releases this device refused because they came from a thread that does not own the
  /// browser device. Test accessor; a nonzero count is a caller error, not a device state.
  uint64_t foreignThreadReleasesForTest() const {
    return foreignThreadReleases_.load(std::memory_order_relaxed);
  }

  /// How many waits this device refused for being entered while it was already yielding. Test
  /// accessor; a nonzero count is a caller error, not a device state.
  uint64_t nestedWaitRefusalsForTest() const { return nestedWaitRefusals_; }

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
                        const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize,
                        const Origin2d& destinationOrigin) override;
  Status onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                  std::span<const Command> commands) override;

  Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex, MapMode mode,
                          uint64_t offsetBytes, uint64_t byteCount) override;
  MapSliceReport onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) override;

  /**
   * Yields the thread to the browser until it reports \p serial complete, the device is lost, or
   * the budget runs out. Nothing completes while this thread holds the event loop, so the wait is
   * spent handing it over rather than resting on it; a wait entered from inside this device's own
   * yield is refused, like the mapping slice path.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds.
   */
  bool onWaitForSerial(uint64_t serial, double timeoutSeconds) override;
  Result<std::span<const uint8_t>> onMappedBytes(uint32_t mappingSlotIndex) const override;
  void onUnmapBuffer(uint32_t mappingSlotIndex) override;

  Status onCreateSurface(uint32_t slotIndex, const SurfaceDescriptor& descriptor) override;
  Result<SurfaceCapabilities> onSurfaceCapabilities(uint32_t slotIndex) const override;
  Status onConfigureSurface(uint32_t slotIndex, const SurfaceConfiguration& configuration) override;
  Result<SurfaceStatus> onAcquireCurrentTexture(uint32_t slotIndex,
                                                uint32_t textureSlotIndex) override;
  Result<SurfaceStatus> onPresentSurface(uint32_t slotIndex) override;
  void onAbandonCurrentTexture(uint32_t slotIndex) override;

private:
  friend class BrowserDeviceRequest;

  /// Constructs the device over a bridge whose device request is already Ready.
  /// @param bridge Bridge to the browser's GPU service.
  explicit BrowserDevice(std::unique_ptr<BrowserBridge> bridge);

  /**
   * Fails closed unless this device can still be used from here: not lost, and called from the
   * context that owns the browser device.
   *
   * @param operation Operation name for the error message.
   */
  Status checkUsable(std::string_view operation) const;

  /**
   * Returns the identifier of the browser object backing (\p kind, \p slotIndex), failing closed
   * when this device has none.
   *
   * @param kind Kind of object expected in the slot.
   * @param slotIndex Runtime slot index.
   * @param operation Operation name for the error message.
   */
  Result<BrowserObjectId> objectFor(BrowserObjectKind kind, uint32_t slotIndex,
                                    std::string_view operation) const;

  /**
   * Mints an identifier for a new object in (\p kind, \p slotIndex), releasing whatever the slot
   * still held on the browser side first.
   *
   * @param kind Kind of object being created.
   * @param slotIndex Runtime slot index the object occupies.
   * @param operation Operation name for the error message.
   */
  Result<BrowserObjectId> registerObject(BrowserObjectKind kind, uint32_t slotIndex,
                                         std::string_view operation);

  /**
   * Releases the browser object backing (\p kind, \p slotIndex), if any, and forgets it.
   *
   * Refused when the caller is not the thread that owns the browser device: the objects belong to
   * the owning context, so naming one from here would ask a worker that does not hold it to
   * release it, which reports nothing the dropped handle could act on and leaves the object in
   * place. The entry is kept instead, so device teardown on the owning thread still frees it, and
   * the refusal is counted for \ref foreignThreadReleasesForTest.
   *
   * @param kind Kind of object to release.
   * @param slotIndex Runtime slot index.
   */
  void releaseObject(BrowserObjectKind kind, uint32_t slotIndex);

  /// Whether the caller is the thread that owns the browser device.
  bool onOwnerThread() const { return std::this_thread::get_id() == ownerThread_; }

  /// Records the texture slot \p surfaceSlotIndex has acquired, growing the per-surface record.
  /// @param surfaceSlotIndex Surface slot. @param textureSlotIndex Acquired texture slot, or
  /// \ref kNoAcquiredTexture to clear it.
  void setAcquiredTexture(uint32_t surfaceSlotIndex, uint32_t textureSlotIndex);

  /// Texture slot \p surfaceSlotIndex has acquired, or \ref kNoAcquiredTexture if none.
  /// @param surfaceSlotIndex Surface slot.
  uint32_t acquiredTexture(uint32_t surfaceSlotIndex) const;

  /// Hands the frame \p surfaceSlotIndex has acquired back to its surface and stops naming it.
  /// @param surfaceSlotIndex Surface slot holding the frame.
  void releaseAcquiredFrame(uint32_t surfaceSlotIndex);

  /// Hands back every frame recorded against \p textureSlotIndex, so no surface goes on naming a
  /// texture slot that is about to change hands.
  /// @param textureSlotIndex Runtime texture slot index.
  void releaseFramesNaming(uint32_t textureSlotIndex);

  /// Replays one validated command onto the bridge, dispatching to the \ref replay overload for
  /// its type.
  /// @param command Recorded command. @param operation Operation name for the error message.
  Status replayCommand(const Command& command, std::string_view operation);

  // One overload per recorded command type, selected by std::visit. A command form with no
  // overload fails to compile, so a stream this backend cannot replay is caught at build time
  // rather than at the end of a chain of type tests.

  /// Replays a render pass beginning. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const BeginRenderPassCommand& command, std::string_view operation);
  /// Replays a render pipeline selection. @param command Recorded command. @param operation
  /// Operation name for the error message.
  Status replay(const SetPipelineCommand& command, std::string_view operation);
  /// Replays a bind group binding. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const SetBindGroupCommand& command, std::string_view operation);
  /// Replays a vertex buffer binding. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const SetVertexBufferCommand& command, std::string_view operation);
  /// Replays an index buffer binding. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const SetIndexBufferCommand& command, std::string_view operation);
  /// Replays a scissor rectangle. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const SetScissorRectCommand& command, std::string_view operation);
  /// Replays a viewport. @param command Recorded command. @param operation Operation name for the
  /// error message.
  Status replay(const SetViewportCommand& command, std::string_view operation);
  /// Replays a draw. @param command Recorded command. @param operation Operation name for the
  /// error message.
  Status replay(const DrawCommand& command, std::string_view operation);
  /// Replays an indexed draw. @param command Recorded command. @param operation Operation name
  /// for the error message.
  Status replay(const DrawIndexedCommand& command, std::string_view operation);
  /// Replays a render pass ending. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const EndRenderPassCommand& command, std::string_view operation);
  /// Replays a compute pass beginning. @param command Recorded command. @param operation
  /// Operation name for the error message.
  Status replay(const BeginComputePassCommand& command, std::string_view operation);
  /// Replays a compute pipeline selection. @param command Recorded command. @param operation
  /// Operation name for the error message.
  Status replay(const SetComputePipelineCommand& command, std::string_view operation);
  /// Replays a workgroup dispatch. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const DispatchWorkgroupsCommand& command, std::string_view operation);
  /// Replays a compute pass ending. @param command Recorded command. @param operation Operation
  /// name for the error message.
  Status replay(const EndComputePassCommand& command, std::string_view operation);
  /// Replays a texture-to-buffer copy. @param command Recorded command. @param operation
  /// Operation name for the error message.
  Status replay(const CopyTextureToBufferCommand& command, std::string_view operation);
  /// Replays a texture-to-texture copy. @param command Recorded command. @param operation
  /// Operation name for the error message.
  Status replay(const CopyTextureToTextureCommand& command, std::string_view operation);

  /// Translates one validated bind group entry, resolving its resource to an identifier.
  /// @param entry Validated entry. @param operation Operation name for the error message.
  Result<BrowserBindGroupEntry> translateBindGroupEntry(const BindGroupEntry& entry,
                                                        std::string_view operation) const;

  /// Translates a validated render pipeline descriptor into the browser's terms, resolving every
  /// referenced object to its identifier.
  /// @param descriptor Validated descriptor. @param request Receives the translation.
  Status buildRenderPipelineRequest(const RenderPipelineDescriptor& descriptor,
                                    BrowserRenderPipelineRequest& request) const;

  /// Translates the vertex buffer layouts of a render pipeline.
  /// @param vertex Validated vertex stage. @param layouts Receives the translation.
  Status buildVertexBuffers(const VertexState& vertex,
                            std::vector<BrowserVertexBufferLayout>& layouts) const;

  /// Translates the color targets of a render pipeline.
  /// @param fragment Validated fragment stage. @param targets Receives the translation.
  Status buildColorTargets(const FragmentState& fragment,
                           std::vector<BrowserColorTarget>& targets) const;

  std::unique_ptr<BrowserBridge> bridge_;

  /// Identifiers of every browser object this device owns, by runtime slot.
  BrowserObjectTable objects_;

  /// Runtime texture slot each surface currently has acquired, indexed by surface slot, with
  /// \ref kNoAcquiredTexture where a surface holds no frame.
  ///
  /// The runtime releases an acquired texture's slot directly rather than through the destruction
  /// hook - a frame texture belongs to the surface, not to the caller's resource table - so
  /// nothing else would tell this backend when to let go of the browser object behind it.
  std::vector<uint32_t> acquiredTextureBySurface_;

  /// Value in \ref acquiredTextureBySurface_ meaning the surface holds no frame.
  static constexpr uint32_t kNoAcquiredTexture = UINT32_MAX;

  /// Whether \p textureSlotIndex is the frame some surface currently holds.
  ///
  /// A frame texture belongs to the canvas that supplied it, so it is handed back rather than
  /// destroyed however its identifier stops being used.
  ///
  /// @param textureSlotIndex Runtime texture slot index.
  bool isAcquiredFrame(uint32_t textureSlotIndex) const;

  /// Releases from a thread that does not own the browser device, counted rather than performed.
  std::atomic<uint64_t> foreignThreadReleases_ = 0;

  /**
   * Longest a single wait slice may hand the thread over for.
   *
   * The slice is a caller-supplied duration the runtime only bounds above zero, so an unbounded one
   * would reach a backend that must express it in a fixed-width unit. Clamping costs nothing: the
   * runtime re-enters this hook until its own budget elapses, so a clamped slice does not shorten
   * the wait, it only gives the browser the thread back more often.
   */
  static constexpr double kMaxYieldSeconds = 1.0;

  /// True while this device has handed the thread to the browser and not yet taken it back.
  bool yielding_ = false;

  /// Waits refused because they were entered while this device was already yielding.
  uint64_t nestedWaitRefusals_ = 0;

  /// Thread that obtained the browser device. Browser objects are unusable off it, so every
  /// operation checks it rather than relying on the runtime's documented affinity alone.
  std::thread::id ownerThread_;
};

}  // namespace donner::gpu::browser
