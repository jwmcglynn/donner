#pragma once
/// @file
/// \c donner::gpu::Device - the abstract GPU device with shared fail-closed validation.

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"
#include "donner/gpu/Commands.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/DeviceObserver.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/Handles.h"
#include "donner/gpu/TextureExport.h"

namespace donner::gpu {

class CommandEncoder;

namespace details {

/**
 * Generation-checked slot storage for one resource type.
 *
 * Slots are reused through a free list; every release bumps the slot's generation so stale
 * handles referencing the previous occupant fail validation.
 *
 * @tparam Record Validated per-resource record stored in each slot.
 */
template <typename Record>
class SlotTable {
public:
  /**
   * Stores \p record in a free slot (reusing released slots first) and returns its index.
   *
   * @param record Record to store.
   */
  uint32_t allocate(Record&& record) {
    if (!freeList_.empty()) {
      const uint32_t slotIndex = freeList_.back();
      freeList_.pop_back();
      Slot& slot = slots_[slotIndex];
      slot.alive = true;
      slot.lastUseSerial = 0;
      slot.record = std::move(record);
      return slotIndex;
    }

    slots_.push_back(
        Slot{.generation = 1, .alive = true, .lastUseSerial = 0, .record = std::move(record)});
    return static_cast<uint32_t>(slots_.size() - 1);
  }

  /**
   * Returns the record at \p slotIndex if it is alive and \p generation is current, otherwise
   * nullptr.
   *
   * @param slotIndex Slot index.
   * @param generation Generation the caller's handle carries.
   */
  const Record* find(uint32_t slotIndex, uint32_t generation) const {
    if (slotIndex >= slots_.size()) {
      return nullptr;
    }
    const Slot& slot = slots_[slotIndex];
    if (!slot.alive || slot.generation != generation) {
      return nullptr;
    }
    return &slot.record.value();
  }

  /**
   * Mutable variant of \ref find, for consuming stored state (e.g. submitting a command buffer).
   *
   * @param slotIndex Slot index.
   * @param generation Generation the caller's handle carries.
   */
  Record* findMutable(uint32_t slotIndex, uint32_t generation) {
    return const_cast<Record*>(std::as_const(*this).find(slotIndex, generation));
  }

  /**
   * Current generation of \p slotIndex.
   *
   * @param slotIndex Slot index; must be in range.
   */
  uint32_t generationOf(uint32_t slotIndex) const { return slots_[slotIndex].generation; }

  /**
   * Releases the record at \p slotIndex immediately: \ref retire plus \ref recycle. Used when the
   * resource was never visible to submissions (creation rollback, command buffer consumption).
   *
   * @param slotIndex Slot index; must refer to an alive slot.
   */
  void release(uint32_t slotIndex) {
    retire(slotIndex);
    recycle(slotIndex);
  }

  /**
   * Retires the record at \p slotIndex: marks the slot dead and bumps its generation so remaining
   * identifiers fail closed, but does NOT queue the slot for reuse. The caller must \ref recycle
   * the slot once the backend object is safe to destroy; until then, no new resource can occupy
   * the slot, so in-flight backend state cannot be aliased.
   *
   * @param slotIndex Slot index; must refer to an alive slot.
   */
  void retire(uint32_t slotIndex) {
    Slot& slot = slots_[slotIndex];
    slot.alive = false;
    slot.record.reset();
    ++slot.generation;
  }

  /**
   * Queues a retired slot for reuse. Called after the backend object is destroyed.
   *
   * @param slotIndex Slot index; must refer to a retired slot.
   */
  void recycle(uint32_t slotIndex) { freeList_.push_back(slotIndex); }

  /**
   * Records that the resource at \p slotIndex is referenced by the submission with
   * \p submissionSerial. Serials increase monotonically, so the stored value is the last use.
   *
   * @param slotIndex Slot index; must refer to an alive slot.
   * @param submissionSerial Serial of the submission referencing the resource.
   */
  void markUsed(uint32_t slotIndex, uint64_t submissionSerial) {
    slots_[slotIndex].lastUseSerial = submissionSerial;
  }

  /**
   * Serial of the last submission referencing \p slotIndex (0 if never submitted). Valid for
   * retired slots too: retiring preserves the last use so deferred destruction can key on it,
   * and for an index the table has never covered, which reads as never submitted.
   *
   * @param slotIndex Slot index.
   */
  uint64_t lastUseOf(uint32_t slotIndex) const {
    return slotIndex < slots_.size() ? slots_[slotIndex].lastUseSerial : 0;
  }

  /**
   * Calls \p callback with the record of every live slot, for the few operations that act on a
   * resource's dependents rather than on the handle a caller named.
   *
   * @param callback Invoked as `callback(Record&)` for each live slot, in slot order.
   */
  template <typename Callback>
  void forEachLive(Callback&& callback) {
    for (Slot& slot : slots_) {
      if (slot.alive) {
        callback(slot.record.value());
      }
    }
  }

  /**
   * Read-only \ref forEachLive, for callers that only inspect the live records.
   *
   * @param callback Invoked as `callback(const Record&)` for each live slot, in slot order.
   */
  template <typename Callback>
  void forEachLive(Callback&& callback) const {
    for (const Slot& slot : slots_) {
      if (slot.alive) {
        callback(slot.record.value());
      }
    }
  }

private:
  /// One slot: generation counter plus the stored record while alive.
  struct Slot {
    uint32_t generation = 1;       //!< Bumped on retire; a handle matches only its generation.
    bool alive = false;            //!< True while the slot holds a live resource.
    uint64_t lastUseSerial = 0;    //!< Serial of the last submission referencing this slot.
    std::optional<Record> record;  //!< Stored record; empty while dead.
  };

  std::vector<Slot> slots_;
  std::vector<uint32_t> freeList_;
};

}  // namespace details

/**
 * Internal: validates a texel copy layout against a copy extent and format, and returns the
 * exclusive end byte offset the copy requires in the buffer or host memory it describes. Shared
 * by `Device::writeTexture` and `CommandEncoder::copyTextureToBuffer`; not part of the public
 * API.
 *
 * @param layout Row layout to validate (256-aligned `bytesPerRow` covering one row, and
 *   `rowsPerImage` covering the copy height).
 * @param copySize Copy extent in texels.
 * @param format Texel format of the texture side of the copy.
 * @param context Operation name for diagnostics.
 */
Result<uint64_t> ValidateTexelCopyInternal(const TexelCopyBufferLayout& layout,
                                           const Extent2d& copySize, TextureFormat format,
                                           std::string_view context);

/// One command buffer of a submission, as a backend receives it: the slot the buffer occupied
/// before it was consumed, plus its validated commands in recording order.
///
/// \ref commands aliases storage the submitting device owns for the duration of the
/// \ref Device::onSubmit call this struct is handed to, and nothing else keeps it alive. A
/// backend that needs the commands after that call returns copies them; it must not retain the
/// span, or this struct, past the call.
struct SubmittedCommandBuffer {
  uint32_t slotIndex = 0;             //!< Slot the command buffer occupied before consumption.
  std::span<const Command> commands;  //!< Validated commands, in recording order.
};

/// A texture acquired from a surface for one frame, with the state the surface reported while
/// handing it over.
///
/// A non-success status can still come with a usable texture: a surface that has drifted out of
/// date usually still presents, so the caller chooses between drawing this frame and
/// reconfiguring first.
struct SurfaceTexture {
  Texture texture;                                //!< The frame's texture; null when none came.
  SurfaceStatus status = SurfaceStatus::Success;  //!< What the surface reported.
};

/**
 * Abstract GPU device: resource creation, queue writes, and submission with shared fail-closed
 * validation.
 *
 * The public API is non-virtual and validates every descriptor, handle, and byte range before
 * delegating to protected `on*` virtuals (template-method pattern), so every backend - the
 * deterministic \ref RecordingDevice and the platform backends - inherits
 * identical fail-closed behavior. Validation runs in release builds too: invalid input returns
 * a \ref GpuError, never asserts.
 *
 * Enforced limits are documented in GpuLimits.h. Handle validation checks device identity
 * (\ref GpuErrorType::DeviceMismatch) and slot generation (\ref GpuErrorType::InvalidHandle), so
 * destroyed or foreign handles fail before reaching a backend.
 *
 * Lifetime: handles are move-only RAII - dropping a live
 * handle releases the resource, an explicit `destroy*(std::move(handle))` call releases it early
 * and reports errors, and device teardown frees everything that remains. A handle that outlives
 * its device releases nothing (its device-alive token has expired). Destruction is deferred by
 * submission serial: destroying a resource makes its handles and references stale immediately,
 * but the backend object is kept alive - and its slot is not reused - until every submission
 * referencing it has completed (\ref completedSerial). Deferred backend releases are processed by
 * \ref poll and opportunistically by `destroy*` and \ref submit.
 *
 * Two resource kinds are intentionally exempt from submission pinning because commands never
 * reference them: \ref PipelineLayout and \ref ShaderModule are consumed at pipeline creation
 * only, so destroying one releases its backend object immediately. Backends must snapshot or
 * retain whatever they need from them when the pipeline is created (Metal's compiled pipeline
 * state retains its functions; the Vulkan backend must do the equivalent when it lands).
 *
 * Thread affinity: a Device and everything created from it must be used from one thread at a
 * time, matching the async-renderer worker ownership model.
 */
class Device {
public:
  /// Destructor; expires the device-alive token (so handles that outlive the device release
  /// nothing) and frees all remaining resources. Backends that submit asynchronously must wait
  /// for in-flight submissions in their own destructor before backend state is torn down.
  virtual ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  /// Process-unique identity of this device (starts at 1, never reused). Baked into every handle
  /// for cross-device validation.
  uint64_t deviceId() const { return deviceId_; }

  /**
   * True once the backend root this device drives has been declared lost, either by the backend's
   * device-lost callback or by a bounded wait giving up on it.
   *
   * Sticky: never resets. The condition belongs to the root rather than to this device, so every
   * device sharing the root reports it - a root that stopped answering has stopped answering all
   * of them. Once lost, rendering output is undefined, readbacks return promptly, and teardown
   * skips GPU waits.
   */
  bool isLost() const { return lostState_->lost.load(std::memory_order_acquire); }

  /**
   * Declares this device's backend root lost because a bounded wait gave up, recording which wait
   * it was and how long it actually ran.
   *
   * The attribution is what turns "rendering stopped" into a diagnosable report, and it is only
   * available at the wait site. Loss stays sticky, and only the call that declares it records an
   * attribution; see \ref DeclareDeviceLostAfterWaitTimeout for why that rule is what keeps a
   * backend-reported loss from being relabelled as a wait timeout. Const because observers treat
   * the condition as shared diagnostic state and the waits that discover a hang run through const
   * accessors.
   *
   * @param site Which bounded wait gave up.
   * @param elapsed Wall time that wait spent before giving up.
   * @param reason Human-readable cause, logged once.
   */
  void markLostAfterWaitTimeout(DeviceLostWaitSite site, std::chrono::milliseconds elapsed,
                                const char* reason) const;

  /// Shader representation accepted by this device. Recording and WebGPU devices use WGSL;
  /// native backends override this so callers select the matching build-time artifact.
  virtual ShaderSourceKind shaderSourceKind() const { return ShaderSourceKind::Wgsl; }

  /// Whether indexed draws through this device honor every value of \p format. Metal and WebGPU
  /// always do; a Vulkan device without `fullDrawIndexUint32` caps 32-bit indices below the full
  /// range, and `RenderPassEncoder::setIndexBuffer` refuses that format on it rather than let a
  /// driver truncate index values. @param format Index format to query.
  virtual bool supportsFullIndexRange(IndexFormat format) const {
    (void)format;
    return true;
  }

  /**
   * Creates a buffer. Fails closed on zero or oversized `byteSize` or empty usage.
   *
   * @param descriptor Validated buffer descriptor.
   */
  Result<Buffer> createBuffer(const BufferDescriptor& descriptor);

  /**
   * Creates a 2D texture. Fails closed on zero or oversized extents, empty usage, or
   * `sampleCount != 1`.
   *
   * @param descriptor Validated texture descriptor.
   */
  Result<Texture> createTexture(const TextureDescriptor& descriptor);

  /**
   * Returns the descriptor \p texture was created with, by value.
   *
   * This is what the device itself validated the texture against, so a caller reading its extent,
   * format or capabilities from here cannot describe the texture differently than the device
   * does. Null, foreign-device, and stale handles fail with the same resource errors as other
   * texture operations, which makes this also the answer to "does this handle still name a
   * texture of mine?".
   *
   * @param texture Live texture owned by this device.
   */
  Result<TextureDescriptor> textureDescriptor(const Texture& texture) const;

  /**
   * Creates a view of \p texture covering the whole texture.
   *
   * @param texture Texture to view; must be a live handle of this device.
   * @param descriptor Validated view descriptor.
   */
  Result<TextureView> createTextureView(const Texture& texture,
                                        const TextureViewDescriptor& descriptor);

  /**
   * Creates a sampler.
   *
   * @param descriptor Validated sampler descriptor.
   */
  Result<Sampler> createSampler(const SamplerDescriptor& descriptor);

  /**
   * Creates a bind group layout. Fails closed on duplicate binding indices, out-of-limit binding
   * counts or indices, or empty per-entry visibility.
   *
   * @param descriptor Validated layout descriptor.
   */
  Result<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDescriptor& descriptor);

  /**
   * Creates a bind group. Fails closed unless every layout binding is matched by exactly one
   * entry whose resource is live, belongs to this device, matches the layout's
   * \ref BindingType, and carries the usage that binding type requires.
   *
   * @param descriptor Validated bind group descriptor.
   */
  Result<BindGroup> createBindGroup(const BindGroupDescriptor& descriptor);

  /**
   * Creates a pipeline layout. Fails closed on out-of-limit group counts or invalid layout
   * references.
   *
   * @param descriptor Validated pipeline layout descriptor.
   */
  Result<PipelineLayout> createPipelineLayout(const PipelineLayoutDescriptor& descriptor);

  /**
   * Creates a shader module from trusted generated source. Fails closed on empty source text.
   * Native Metal also requires engaged buffer-binding metadata before compiling any MSL module;
   * an explicitly empty list declares that the module uses no buffers.
   *
   * @param descriptor Validated shader module descriptor.
   */
  Result<ShaderModule> createShaderModule(const ShaderModuleDescriptor& descriptor);

  /**
   * Creates a render pipeline. Fails closed on invalid layout/module references, empty entry
   * points, vertex attributes that overflow their stride, duplicate shader locations, empty or
   * out-of-limit target lists, or `multisampleCount != 1`.
   *
   * @param descriptor Validated pipeline descriptor.
   */
  Result<RenderPipeline> createRenderPipeline(const RenderPipelineDescriptor& descriptor);

  /**
   * Creates a compute pipeline. Fails closed on invalid layout/module references, an empty entry
   * point, or a workgroup size that is zero in any dimension, exceeds a per-axis cap, or declares
   * more than \ref kMaxComputeInvocationsPerWorkgroup invocations.
   *
   * @param descriptor Validated pipeline descriptor.
   */
  Result<ComputePipeline> createComputePipeline(const ComputePipelineDescriptor& descriptor);

  // The destroy* methods consume the handle: on success it becomes stale immediately (along with
  // all references to it) while the backend object is deferred until in-flight submissions
  // complete; on failure the handle is still consumed, and if it named a live resource of a
  // DIFFERENT device, that resource is released through RAII on its owning device (the error is
  // still reported). Either way the caller's handle is null afterwards, so explicit destroy plus
  // RAII can never double-free.

  /// Destroys a buffer (see the destroy contract above). @param buffer Handle to destroy.
  Status destroyBuffer(Buffer&& buffer);
  /// Destroys a texture (see the destroy contract above). @param texture Handle to destroy.
  Status destroyTexture(Texture&& texture);
  /// Destroys a texture view (see the destroy contract above).
  /// @param textureView Handle to destroy.
  Status destroyTextureView(TextureView&& textureView);
  /// Destroys a sampler (see the destroy contract above). @param sampler Handle to destroy.
  Status destroySampler(Sampler&& sampler);
  /// Destroys a bind group layout (see the destroy contract above).
  /// @param bindGroupLayout Handle to destroy.
  Status destroyBindGroupLayout(BindGroupLayout&& bindGroupLayout);
  /// Destroys a bind group (see the destroy contract above). @param bindGroup Handle to destroy.
  Status destroyBindGroup(BindGroup&& bindGroup);
  /// Destroys a pipeline layout (see the destroy contract above).
  /// @param pipelineLayout Handle to destroy.
  Status destroyPipelineLayout(PipelineLayout&& pipelineLayout);
  /// Destroys a shader module (see the destroy contract above).
  /// @param shaderModule Handle to destroy.
  Status destroyShaderModule(ShaderModule&& shaderModule);
  /// Destroys a render pipeline (see the destroy contract above).
  /// @param renderPipeline Handle to destroy.
  Status destroyRenderPipeline(RenderPipeline&& renderPipeline);
  /// Destroys a compute pipeline (see the destroy contract above).
  /// @param computePipeline Handle to destroy.
  Status destroyComputePipeline(ComputePipeline&& computePipeline);

  /**
   * Destroys \p buffer and releases its backend allocation at once, rather than leaving the
   * allocation to the backend's own collector.
   *
   * \ref destroyBuffer ends the handle; on a backend that reference-counts its allocations, the
   * memory behind it stays resident until whatever else still names it lets go. A readback buffer
   * whose map was cancelled, and a pooled readback set evicted to keep a pool inside its ceiling,
   * both have to give their memory back at that moment instead of whenever a collector next runs,
   * so this operation says so explicitly.
   *
   * \warning Releasing now is only safe once no unfinished submission still reads the buffer,
   * which is a fact only the caller has: the ordinary \ref destroyBuffer defers the release until
   * the last submission naming the resource completes, and this operation gives that up in
   * exchange for reclaiming the memory immediately. Callers satisfy it by construction - a
   * cancelled map has no submission left to finish, and a pooled entry is idle before it can be
   * evicted.
   *
   * The handle is validated before anything is freed, so a stale, foreign, or already-destroyed
   * handle is reported and reaches nothing - in particular not whatever now occupies its slot.
   * The destroy contract above still applies in full: the handle is consumed either way, and one
   * naming a live resource of another device is released on that device.
   *
   * @param buffer Live buffer handle of this device; consumed either way.
   */
  Status destroyBufferBacking(Buffer&& buffer);

  /**
   * Destroys \p texture and releases its backend allocation at once, the texture counterpart of
   * \ref destroyBufferBacking; a succession of resized render targets is where the deferred
   * release shows up as retained memory. The same caller precondition applies.
   *
   * A texture whose backing this device does not own (see \ref ownsTextureBacking) keeps its
   * allocation: the owner is whoever registered it or handed the frame out, and destroying the
   * handle only forgets this device's name for it. That is enforced here rather than left to each
   * backend, so the guarantee holds for every one of them.
   *
   * @param texture Live texture handle of this device; consumed either way.
   */
  Status destroyTextureBacking(Texture&& texture);

  /**
   * Whether \p texture names backing this device allocated, rather than memory some other owner
   * holds and this device only named.
   *
   * False for a null, stale, or foreign handle, so this answers "may I take this texture's
   * allocation over?" rather than "does this handle resolve?". It is also false for a frame
   * acquired from a surface: the presentation surface hands that texture out and takes it back,
   * and a caller that freed it would free memory the swapchain still owns.
   *
   * @param texture Handle to inspect; its device and generation are validated first.
   */
  [[nodiscard]] bool ownsTextureBacking(const Texture& texture) const;

  /**
   * Exports \p texture so another runtime device over the same backend device can register it.
   *
   * Runs on this device's thread, like every other operation that reads its table: the handle is
   * resolved here, so a null or stale handle fails with \ref GpuErrorType::InvalidHandle and one of
   * another device with \ref GpuErrorType::DeviceMismatch. Nothing is allocated from the backend,
   * nothing is submitted, and nothing waits.
   *
   * The token holds the texture's allocation: it stays alive while the token, or any registration
   * made from it, is alive, including after this device releases its handle.
   *
   * A frame a surface currently has out goes back to that surface when it is presented. It is
   * exported only by a backend whose registrations submit to this device's own queue
   * (\ref SourceOrdering::SharedQueue): a reader's work recorded before the present then runs
   * before it. The caller must not record reads of such a registration after the frame is
   * presented. A backend whose readers have their own queues refuses the frame with
   * \ref GpuErrorType::InvalidState, because their reads could land after the surface took it
   * back.
   *
   * Also refused with \ref GpuErrorType::InvalidState for a registration of another device's
   * texture, which is exported from the device that allocated it; with
   * \ref donner::gpu::GpuErrorType::DeviceLost "GpuErrorType::DeviceLost" once this device is lost;
   * and with \ref GpuErrorType::Unsupported by a backend whose runtime devices never share a native
   * device.
   *
   * @param texture Live texture of this device.
   */
  Result<TextureExport> exportTexture(const Texture& texture);

  /**
   * Registers a texture another runtime device exported, as a read-only texture of this one.
   *
   * Runs on this device's thread and reads only \p source, never the producer device. The
   * registration describes the texture as the producer does, except that its usage is limited to
   * \ref TextureUsage::Sampled and \ref donner::gpu::TextureUsage::CopySrc "TextureUsage::CopySrc":
   * a consumer reads what the producer wrote and never writes it. It never owns the allocation
   * (\ref ownsTextureBacking is false) but holds it until this device recycles the registration's
   * slot, which is after the last of this device's submissions naming it has completed.
   *
   * Work this device submits that names the registration is ordered after every submission the
   * producer had made referencing the texture before this call, and after the queued writes those
   * submissions carried. On a backend whose devices do not share one native queue, either the
   * backend orders such work on the device (\ref SourceOrdering::WaitOnDevice) for a producer
   * that shares this device's loss condition, so \ref submit accepts it once the producer has
   * handed that work to its queue and the work waits on the device, or \ref submit refuses it
   * until that producer work has completed. Either way \ref waitForTextureSource is the host's
   * bounded wait until the work may be submitted.
   *
   * Refused with \ref GpuErrorType::InvalidHandle for an empty token or one whose producer has
   * released its handle; \ref GpuErrorType::InvalidState for this device's own export or while a
   * producer write to the texture is queued and not yet submitted; \ref
   * GpuErrorType::DeviceMismatch for another backend or native device; \ref
   * GpuErrorType::UsageMismatch when the texture can be neither sampled nor copied from;
   * \ref donner::gpu::GpuErrorType::DeviceLost "GpuErrorType::DeviceLost" when either device is
   * lost; and \ref GpuErrorType::Unsupported by a backend that cannot name another device's
   * textures.
   *
   * @param source Token from the producer's \ref exportTexture.
   */
  Result<Texture> registerTexture(const TextureExport& source);

  /**
   * Waits until work naming \p registration may be submitted, the budget runs out, or either
   * device is lost, and reports whether it may.
   *
   * Work may be submitted once the producer work the registration is ordered after has completed.
   * Returns true at once for a texture this device allocated and for a registration whose
   * producer shares this device's native queue, because submission order already orders them,
   * and for a registration ordered on the device as soon as its producer has handed that work to
   * its queue, because the device then orders it. A registration is ordered on the device only
   * when its producer shares this device's loss condition: the device-side wait ends however the
   * producer's work ends, and only a shared condition carries a failure or loss there to this
   * device. A wait that spends its budget declares nothing,
   * like \ref waitForSerial; the caller's deadline is its own policy. A producer whose backend
   * reports a terminal execution failure is declared lost with no wait site, because the backend
   * reported it and no deadline expired.
   *
   * @param registration Live texture of this device.
   * @param timeoutSeconds Longest to wait, in seconds; clamped like \ref waitForSerial.
   * @return True once work naming the registration may be submitted.
   */
  bool waitForTextureSource(const Texture& registration, double timeoutSeconds);

  /**
   * Bytes of this device's exported textures that are still held by an export token or a
   * registration after this device released its own handle.
   *
   * That memory is still resident, and no other device counts it as its allocation, so a working
   * set measured from allocation accounting alone would miss it. Readable from any thread. The
   * gauge belongs to this device: bytes still held after this device is destroyed are counted in a
   * gauge nothing reads any more, so a holder that outlives its producer device is a working-set
   * blind spot.
   */
  [[nodiscard]] uint64_t sharedTextureTailBytes() const;

  /// Creates a command encoder recording against this device. The encoder must not outlive the
  /// device.
  Result<std::unique_ptr<CommandEncoder>> createCommandEncoder();

  /**
   * Creates a surface presenting to a platform object.
   *
   * The surface is not usable until \ref configureSurface succeeds: a surface knows what it
   * presents to, and a configuration knows how.
   *
   * @param descriptor Label and platform object to present to.
   */
  Result<Surface> createSurface(const SurfaceDescriptor& descriptor);

  /// What \p surface supports, for choosing a configuration. @param surface Live surface.
  Result<SurfaceCapabilities> surfaceCapabilities(const Surface& surface) const;

  /**
   * Configures how \p surface presents, replacing any previous configuration.
   *
   * This is how a surface follows its window: a resize reconfigures with the new extent rather
   * than creating a new surface, and it is the recovery from \ref SurfaceStatus::Outdated.
   * Reconfiguring abandons any texture currently acquired from the surface.
   *
   * @param surface Live surface.
   * @param configuration Format, usage, extent, pacing and alpha compositing.
   */
  Status configureSurface(const Surface& surface, const SurfaceConfiguration& configuration);

  /**
   * Acquires the texture for the next frame of \p surface.
   *
   * The texture is valid until the matching \ref presentSurface, \ref abandonCurrentTexture, or
   * a reconfiguration - each of those invalidates it, so drawing into a presented texture is a
   * reported validation failure rather than a write to a texture the platform now owns.
   *
   * A non-success status can still come with a usable texture: a surface that has drifted out of
   * date usually still presents, so the caller decides between drawing this frame and
   * reconfiguring first.
   *
   * @param surface Live, configured surface.
   */
  Result<SurfaceTexture> acquireCurrentTexture(const Surface& surface);

  /**
   * Presents the texture acquired from \p surface and invalidates it.
   *
   * Not every platform lets a caller present: where the host drives its own frame loop, the
   * frame appears when the host shows it, and asking to present is rejected rather than
   * performed. Such a surface still acquires and still invalidates its texture at the end of the
   * frame through \ref abandonCurrentTexture.
   *
   * @param surface Live surface with an acquired texture.
   */
  Result<SurfaceStatus> presentSurface(const Surface& surface);

  /**
   * Releases the acquired texture of \p surface without presenting it, for a frame the caller
   * decided not to show.
   *
   * @param surface Live surface with an acquired texture.
   */
  Status abandonCurrentTexture(const Surface& surface);

  /// Destroys a surface (see the destroy contract above). @param surface Handle to destroy.
  Status destroySurface(Surface&& surface);

  /**
   * Begins mapping a range of \p buffer for host access, and returns the handle that names the
   * mapping.
   *
   * The mapping is not readable yet: \ref donner::gpu::Device::waitForMapping decides when it is.
   * The returned handle is what keeps the mapped range reachable - \ref unmapBuffer consumes it and
   * every copy of it goes stale at that moment, so a read through a handle whose mapping has been
   * released is a reported validation failure rather than a read of memory that is no longer there.
   *
   * A buffer carries at most one mapping at a time: a second request while one is open is
   * refused with \ref GpuErrorType::InvalidState. Two mappings of one buffer would each be
   * released by the other's \ref unmapBuffer, so one handle would decide when another handle's
   * bytes went away.
   *
   * Holding a mapping is not ownership of the buffer. Destroying the buffer is allowed while a
   * mapping is open, and invalidates it: the handle stays resolvable and reads through it are
   * refused, rather than the buffer being kept alive by a reader that has not finished.
   *
   * @param buffer Buffer to map; needs \ref BufferUsage::MapRead.
   * @param mode How the host will access the range.
   * @param offsetBytes Byte offset of the mapped range.
   * @param byteCount Length of the mapped range; must be nonzero and fit the buffer.
   */
  Result<BufferMapping> mapBufferAsync(const Buffer& buffer, MapMode mode, uint64_t offsetBytes,
                                       uint64_t byteCount);

  /// Test seam for \ref donner::gpu::Device::waitForMapping. Production callers pass none; a test
  /// injects a clock and a rest so a budget is verified deterministically and without spending it.
  struct MapWaitTestHooks {
    /// Clock the budget is measured against. Defaults to `std::chrono::steady_clock::now`.
    std::function<std::chrono::steady_clock::time_point()> now;
    /// Rests for the given duration. Defaults to `std::this_thread::sleep_for`.
    std::function<void(std::chrono::microseconds)> rest;
  };

  /**
   * Waits for a pending mapping in slices, until it completes, the caller stops it, the budget
   * runs out, or the device is lost.
   *
   * \p shouldCancel is consulted before each slice, so a caller that changes its mind stops
   * within one slice rather than at the end of the budget. Device loss ends the wait
   * immediately: the mapping can never complete afterwards, and reporting it as a timeout would
   * describe a permanent failure as a slow one.
   *
   * Destroying the mapped buffer is reported by where the wait was when it happened: a buffer
   * already gone when the wait starts fails the call with \ref GpuErrorType::InvalidHandle, while
   * one destroyed during the wait ends it with \ref MapWaitOutcome::Failed, because by then the
   * call has a wait to report the outcome of rather than a handle to reject.
   *
   * The report carries how the backend spent the wait alongside how it ended, because a wait that
   * blocked on the map's completion signal and one that polled for it take wall times orders of
   * magnitude apart, and only the backend knows which it did.
   *
   * @param mapping Live mapping of this device.
   * @param params Slice length and total budget; both must be greater than zero.
   * @param shouldCancel Consulted before each slice; may be empty for an uncancellable wait.
   * @param testHooks Optional clock/rest hooks for deterministic tests; production passes none.
   */
  Result<MapWaitReport> waitForMapping(const BufferMapping& mapping, const MapWaitParams& params,
                                       const std::function<bool()>& shouldCancel,
                                       const MapWaitTestHooks& testHooks = {});

  /**
   * Returns the mapped bytes of a completed mapping.
   *
   * Fails closed when the mapping is stale, belongs to another device, has not completed, named
   * a buffer that has since been destroyed, or belongs to a device that has been lost
   * (\ref donner::gpu::GpuErrorType::DeviceLost "GpuErrorType::DeviceLost"): the span is only valid
   * while the handle names a live, ready mapping. Completion means a \ref
   * donner::gpu::Device::waitForMapping on this mapping reported \ref MapWaitOutcome::Ready; until
   * one has, reading is refused rather than racing whatever the GPU is still writing.
   *
   * The span aliases the backend's allocation rather than a copy of it, so it lives only as long
   * as the mapping does: \ref unmapBuffer, destroying the buffer, or losing the device all end
   * it. A caller that keeps the bytes past any of those copies them out first. The
   * \c UTILS_LIFETIME_BOUND annotation states that contract to the compiler; it is not relied on
   * to catch every misuse, because the span is returned inside a \ref Result and the dangling
   * diagnostic does not see through an unannotated class template.
   *
   * @param mapping Live, completed mapping of this device.
   */
  Result<std::span<const uint8_t>> mappedBytes(const BufferMapping& mapping) const
      UTILS_LIFETIME_BOUND;

  /**
   * Releases a mapping, invalidating the handle and every copy of it.
   *
   * @param mapping Mapping to release; consumed either way (see the destroy contract above).
   */
  Status unmapBuffer(BufferMapping&& mapping);

  /**
   * Writes \p data into \p buffer at \p offsetBytes. Fails closed if the range does not fit
   * (checked arithmetic) or the buffer lacks \ref BufferUsage::CopyDst.
   *
   * A write never changes bytes an already submitted command still reads, and for the same reason
   * never changes bytes the host is reading: a buffer with an open mapping is refused with
   * \ref GpuErrorType::InvalidState until \ref unmapBuffer releases it.
   *
   * MetalDevice and
   * VulkanDevice copy busy-buffer writes with four-byte-aligned offsets and sizes into a bounded
   * queue, flushed before the next ordinary submission, including an empty command stream.
   * Unaligned writes wait for that buffer's outstanding work and return
   * GpuErrorType::InvalidState if the bounded wait times out. On VulkanDevice, a wait that ends
   * because another device over the same root declared it lost returns GpuErrorType::DeviceLost
   * instead. Queued writes can fail with GpuErrorType::LimitExceeded when their staging budget
   * is exhausted. Callers must handle these errors without assuming a failed write changed the
   * buffer.
   *
   * @param buffer Destination buffer.
   * @param offsetBytes Destination byte offset.
   * @param data Payload bytes.
   */
  Status writeBuffer(const Buffer& buffer, uint64_t offsetBytes, std::span<const uint8_t> data);

  /**
   * Writes texel rows from \p data into the \p writeSize rectangle of \p texture whose top-left
   * texel is \p destinationOrigin, leaving every texel outside that rectangle unchanged. The
   * origin defaults to texel (0, 0), the whole-rect-from-the-origin convention this operation had
   * before sub-rectangle writes existed, so an existing call keeps its meaning.
   *
   * Fails closed unless the texture has \ref TextureUsage::CopyDst, \p dataLayout is 256-aligned
   * and covers \p writeSize, \p destinationOrigin plus \p writeSize fits inside the texture,
   * and the described rows fit inside \p data (all checked arithmetic).
   *
   * On VulkanDevice, when another device over the same root declared it lost and this device has
   * no error of its own, a write returns GpuErrorType::DeviceLost without starting an upload, as
   * does an upload whose wait that declaration ends. A device with an error of its own, a
   * driver-reported loss included, returns that error as GpuErrorType::InvalidState.
   *
   * @param texture Destination texture.
   * @param data Payload bytes laid out per \p dataLayout.
   * @param dataLayout Row layout of \p data.
   * @param writeSize Extent to write in texels.
   * @param destinationOrigin Top-left texel of the written rectangle.
   */
  Status writeTexture(const Texture& texture, std::span<const uint8_t> data,
                      const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize,
                      const Origin2d& destinationOrigin = {});

  /// Most command buffers one submission may carry.
  ///
  /// A backend acquires a native command buffer per element and commits none of them until the
  /// whole span has encoded, so that nothing reaches the queue when an element fails to encode.
  /// Native queues cap how many command buffers may be outstanding at once and block the
  /// acquiring thread at that cap, so a span large enough to reach it on its own would wait for
  /// buffers only it could release.
  ///
  /// This is the refusal bound, not a promise that every smaller span clears that cap on every
  /// backend: the cap belongs to the native queue, and a backend layered over another API can sit
  /// far below this one. A caller splitting a frame picks a bound its backend tolerates and stays
  /// under it; this bound only keeps a span from being absurd.
  static constexpr size_t kMaxCommandBuffersPerSubmission = 256;

  /**
   * Submits a finished command buffer, consuming it, and returns the assigned submission serial.
   * Serials start at 1 and increase by 1 per submission; a submission rejected here or by the
   * backend does not consume a serial.
   *
   * Every resource a recorded command references - including bind-group entry resources and the
   * textures behind attachment and entry views - is re-validated against its recorded
   * (slot, generation) identity, so a resource destroyed between recording and submission fails
   * closed with \ref GpuErrorType::InvalidHandle instead of reaching a backend. On success those
   * resources are marked in-use by the new serial, which defers their backend destruction until
   * the submission completes.
   *
   * A buffer with an open mapping is refused with \ref GpuErrorType::InvalidState, and the
   * command buffer is consumed either way, so the work is recorded again after
   * \ref unmapBuffer. This covers uses that only read the buffer as well: the mapped range
   * aliases the buffer's own storage, and a reader cannot tell which parts of it a submission
   * will touch, so the buffer belongs either to the host or to the device and not to both at
   * once.
   *
   * @param commandBuffer Command buffer to submit; consumed even on failure.
   */
  Result<uint64_t> submit(CommandBuffer commandBuffer);

  /**
   * Submits finished command buffers as one submission, consuming them, and returns the single
   * assigned submission serial.
   *
   * The buffers execute in the order they appear in \p commandBuffers, and the whole span is one
   * backend submission with one completion: a caller that records a frame through several
   * independently-owned encoders keeps the frame's ordering and its single completion without
   * reaching a queue per encoder. Splitting a frame across buffers is therefore free to follow
   * whatever bound a backend needs - a very large single command buffer stalls the Metal
   * completion path - without changing what a frame costs on the queue.
   *
   * A span holds between one and \ref kMaxCommandBuffersPerSubmission buffers. Both size
   * refusals carry \ref GpuErrorType::InvalidDescriptor and are decided before anything is
   * consumed, so the caller still owns its buffers: an empty span names no work and would consume
   * a serial nothing can wait on, and an oversized one is refused rather than submitted so the
   * caller splits it across submissions instead.
   *
   * Once consumption starts every buffer is consumed, including after a refusal, so a span that
   * was refused partway cannot be submitted again with the accepted part already gone. Refusals
   * otherwise carry the same meanings as the single-buffer overload.
   *
   * @param commandBuffers Command buffers to submit in execution order; every element is
   *   consumed, leaving the caller's handles null, unless the span's size refuses it outright.
   */
  Result<uint64_t> submit(std::span<CommandBuffer> commandBuffers);

  /**
   * Processes deferred destructions: releases the backend object of every destroyed resource
   * whose last referencing submission has completed (\ref completedSerial), and recycles its
   * slot. Called opportunistically by `destroy*` and \ref submit; call it directly after waiting
   * for completion to reclaim resources promptly. A backend that delivers completion callbacks
   * only while polled drives one nonblocking backend iteration before reclaiming slots.
   */
  void poll();

  /// Whether a completed submission can still release a backend object. Owner-thread only.
  [[nodiscard]] bool hasPendingDestroys() const { return !pendingDestroys_.empty(); }

  /// Serial assigned to the most recent submission (0 if none yet).
  uint64_t lastSubmittedSerial() const { return lastSubmittedSerial_; }

  /// Serial of the most recent submission the backend has finished executing (0 if none). The
  /// recording backend completes instantly, so this equals \ref lastSubmittedSerial there.
  virtual uint64_t completedSerial() const = 0;

  /// Longest a single \ref waitForSerial may block, in seconds. A budget above this is clamped to
  /// it; the value is far past any deadline a caller has, and keeps the conversion to the clock's
  /// own duration from overflowing on a caller that means "wait indefinitely" and says so with a
  /// very large number.
  static constexpr double kMaxWaitSeconds = 1.0e6;

  /**
   * Blocks until \ref completedSerial reaches \p serial, \p timeoutSeconds elapses, or the
   * backend gives up on the work, and reports whether the submission completed.
   *
   * The budget is the caller's and it is always bounded: a wait never outlives it, so a driver
   * that stops answering costs one deadline rather than the process. A backend that can tell it
   * has failed terminally returns false at once instead of spending the budget, because no
   * submission can complete afterwards and reporting that as a timeout would describe a permanent
   * failure as a slow one.
   *
   * A false return is therefore "not completed, and not within this budget", never "completed but
   * unreadable"; a caller that needs to tell a timeout from a dead device asks the backend which
   * it was.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds; clamped to the range zero to
   *   \ref kMaxWaitSeconds, so a budget of zero reports what is already known without blocking.
   * @return True once this device has completed \p serial.
   */
  bool waitForSerial(uint64_t serial, double timeoutSeconds);

  /**
   * Installs \p observer to be notified of the work this device accepts (see
   * \ref DeviceObserver).
   *
   * A device carries at most one observer. Installing the one already installed is accepted;
   * installing a different one while another is installed is refused with
   * \ref GpuErrorType::InvalidState and changes nothing, because replacing it would leave its
   * owner counting nothing while every bound checked against those counts passed.
   *
   * Non-owning: the caller keeps \p observer alive until it removes it with \ref removeObserver.
   * With none installed, each operation costs one null check.
   *
   * @param observer Observer to notify.
   */
  [[nodiscard]] Status installObserver(DeviceObserver& observer);

  /**
   * Removes \p observer when it is the installed one and otherwise changes nothing, so an owner
   * can only ever remove its own.
   *
   * @param observer Observer its owner installed.
   */
  void removeObserver(const DeviceObserver& observer);

  /// The installed observer, or null when there is none.
  const DeviceObserver* observer() const { return observer_; }

protected:
  /// Constructor for backends; assigns the process-unique device identity.
  Device();

  /// One nonblocking backend event iteration before a pending-destroy scan. Native devices
  /// report completion directly; callback-driven adapters override this hook.
  virtual void onPollBackend() {}

  /**
   * Adopts the sticky loss condition of a backend root this device shares with another device.
   *
   * Called by a backend at construction, before anything can observe the device. A root drives
   * several runtime devices - a UI context, a worker context, an isolated readback context - and
   * they must agree on whether it has stopped answering, so the second and later devices over one
   * root take the condition the first one published into rather than minting their own.
   *
   * @param state Condition to share; ignored when null, so a backend may pass an absent one.
   */
  void adoptLostState(std::shared_ptr<DeviceLostState> state);

  /// Last accepted submission referencing a buffer slot already validated by the caller.
  /// @param slotIndex A validated live buffer slot.
  uint64_t bufferLastUseSerial(uint32_t slotIndex) const;

  /// Last accepted submission referencing a texture slot already validated by the caller.
  /// @param slotIndex A validated live texture slot.
  uint64_t textureLastUseSerial(uint32_t slotIndex) const;

  /// Backend hook: a buffer passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) = 0;
  /// Backend hook: a texture passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) = 0;
  /// Backend hook: a texture view passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource.
  /// @param textureSlotIndex Slot index of the viewed texture.
  /// @param descriptor Validated descriptor.
  virtual Status onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                     const TextureViewDescriptor& descriptor) = 0;
  /// Backend hook: a sampler passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) = 0;
  /// Backend hook: a bind group layout passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateBindGroupLayout(uint32_t slotIndex,
                                         const BindGroupLayoutDescriptor& descriptor) = 0;
  /// Backend hook: a bind group passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) = 0;
  /// Backend hook: a pipeline layout passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreatePipelineLayout(uint32_t slotIndex,
                                        const PipelineLayoutDescriptor& descriptor) = 0;
  /// Backend hook: a shader module passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateShaderModule(uint32_t slotIndex,
                                      const ShaderModuleDescriptor& descriptor) = 0;
  /// Backend hook: a render pipeline passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateRenderPipeline(uint32_t slotIndex,
                                        const RenderPipelineDescriptor& descriptor) = 0;
  /// Backend hook: a compute pipeline passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateComputePipeline(uint32_t slotIndex,
                                         const ComputePipelineDescriptor& descriptor) = 0;

  /// Backend hook: a buffer handle was retired. Discard unsubmitted work, but keep native
  /// resources alive until \ref onDestroyResource. @param slotIndex Retired buffer slot.
  virtual void onRetireBuffer(uint32_t slotIndex);

  /// Backend hook: a texture handle was retired. Discard unsubmitted work, but keep native
  /// resources alive until \ref onDestroyResource. @param slotIndex Retired texture slot.
  virtual void onRetireTexture(uint32_t slotIndex);

  /// Backend hook: a retired resource is no longer used by submitted work and can be released.
  /// @param resourceName Resource type name, e.g. `"buffer"`.
  /// @param slotIndex Slot index of the destroyed resource.
  virtual void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) = 0;

  /**
   * Backend hook: \ref destroyBufferBacking validated \p slotIndex and is about to destroy its
   * handle; release the allocation now rather than when the slot is recycled.
   *
   * The default does nothing, which is the whole answer for a backend that already frees the
   * allocation as soon as the last submission naming it completes - there is nothing left to give
   * back earlier, and a backend must never free memory a submitted command still reads.
   *
   * @param slotIndex Validated live buffer slot.
   */
  virtual void onDestroyBufferBacking(uint32_t slotIndex);

  /// Backend hook: the texture counterpart of \ref onDestroyBufferBacking, with the same default.
  /// A slot whose backing this device does not own keeps its allocation.
  /// @param slotIndex Validated live texture slot.
  virtual void onDestroyTextureBacking(uint32_t slotIndex);

  /**
   * Backend hook: whether \p slotIndex holds backing this device allocated.
   *
   * Asked for a validated slot that is not a surface's acquired frame, from two places: the
   * public \ref ownsTextureBacking, and \ref createTexture immediately after \ref onCreateTexture
   * accepted the slot, to decide whether the creation is an allocation an observer counts, both
   * when it is created and when it is released. The answer must therefore already hold when
   * \ref onCreateTexture returns. The default is true: every texture a backend holds is one it
   * created. A backend that can also name memory belonging to someone else - a registration of a
   * host-owned object, say - overrides this and says which is which.
   *
   * @param slotIndex Validated live texture slot.
   */
  [[nodiscard]] virtual bool onOwnsTextureBacking(uint32_t slotIndex) const;

  /**
   * Backend hook: which backend family this device belongs to and which native device it records
   * against, for matching an export against a registering device.
   *
   * The default names nothing, which is the answer for a backend whose runtime devices never share
   * a native device.
   */
  [[nodiscard]] virtual BackendDeviceIdentity backendDeviceIdentity() const;

  /**
   * Backend hook: export the texture in \p slotIndex, which the runtime has already validated as a
   * live texture this device allocated and no surface has out.
   *
   * The default refuses with \ref donner::gpu::GpuErrorType::Unsupported
   * "GpuErrorType::Unsupported": a backend that reaches each native device through exactly one
   * runtime device has no other device that could name the texture.
   *
   * @param slotIndex Validated live texture slot.
   */
  virtual Result<BackendTextureExport> onExportTexture(uint32_t slotIndex);

  /**
   * Backend hook: name another device's exported texture in \p slotIndex.
   *
   * Called only after the runtime has matched \p backing's device identity against
   * \ref backendDeviceIdentity, so a backend may treat \p backing as its own export type. The
   * backend must hold the native object until \ref onDestroyResource for the slot, and must not
   * report it as an allocation of this device.
   *
   * @param slotIndex Slot the registration occupies.
   * @param backing The producer backend's export.
   */
  virtual Status onRegisterTexture(uint32_t slotIndex, const ExportedTextureBacking& backing);

  /**
   * Backend hook: whether the write \ref writeTexture just accepted for \p slotIndex is waiting
   * for this device's next submission, rather than complete or ordered on the queue already.
   *
   * Asked only for an exported texture. The default, false, is right for a backend whose writes
   * either finish before returning or are ordered on the one queue every consumer shares.
   *
   * @param slotIndex Validated live texture slot.
   */
  [[nodiscard]] virtual bool onTextureWritePending(uint32_t slotIndex) const;

  /**
   * Backend hook: \ref waitForSerial with a budget already clamped to zero or more.
   *
   * The default rechecks \ref completedSerial until the budget runs out, resting between checks,
   * which is what a backend whose completions arrive on their own thread needs and nothing more.
   * A backend that can block on a completion signal, or that knows it has failed terminally,
   * overrides this.
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Longest to wait, in seconds; already clamped to a usable range.
   * @return True once this device has completed \p serial.
   */
  virtual bool onWaitForSerial(uint64_t serial, double timeoutSeconds);

  /// Backend hook: a validated buffer write.
  /// @param slotIndex Destination buffer slot. @param offsetBytes Destination byte offset.
  /// @param data Payload bytes.
  virtual Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                               std::span<const uint8_t> data) = 0;
  /// Backend hook: a validated texture write. The written rectangle is \p writeSize texels
  /// wide and tall with its top-left texel at \p destinationOrigin; texels outside it keep their
  /// contents.
  /// @param slotIndex Destination texture slot. @param data Payload bytes.
  /// @param dataLayout Row layout of \p data. @param writeSize Extent written in texels.
  /// @param destinationOrigin Top-left texel of the written rectangle.
  virtual Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize,
                                const Origin2d& destinationOrigin) = 0;

  /**
   * Validates a buffer handle for backend-provided auxiliary entry points (test readback
   * helpers and similar), running the same null/device-identity/generation checks the
   * template-method public API performs before its hooks.
   *
   * @param buffer Handle to validate.
   */
  Status validateBufferHandleForBackend(const Buffer& buffer) const;

  /**
   * Validates a texture handle for backend-provided auxiliary entry points, running the same
   * null/device-identity/generation checks the template-method public API performs.
   *
   * @param texture Handle to validate.
   */
  Status validateTextureHandleForBackend(const Texture& texture) const;

  /**
   * Validates a texture view handle for backend-provided auxiliary entry points: the view
   * itself plus a re-resolution of its viewed texture, so a view of a destroyed (or
   * slot-recycled) texture fails closed exactly like it does on the normal Device paths.
   *
   * @param textureView Handle to validate.
   */
  Status validateTextureViewHandleForBackend(const TextureView& textureView) const;

  /**
   * Validates a buffer-mapping handle for backend-provided auxiliary entry points, running the
   * same null/device-identity/generation checks the template-method public API performs, so a
   * stale handle cannot read state belonging to the slot's new occupant.
   *
   * @param mapping Handle to validate.
   */
  Status validateBufferMappingHandleForBackend(const BufferMapping& mapping) const;

  /**
   * Backend hook: begin mapping a buffer range. Defaults to reporting the capability as
   * unsupported, so a backend without host mapping needs no implementation and callers get a
   * clean unsupported result rather than a missing symbol.
   *
   * @param mappingSlotIndex Slot the mapping occupies.
   * @param bufferSlotIndex Slot of the buffer being mapped.
   * @param mode How the host will access the range.
   * @param offsetBytes Byte offset of the mapped range.
   * @param byteCount Length of the mapped range.
   */
  virtual Status onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex, MapMode mode,
                                  uint64_t offsetBytes, uint64_t byteCount);

  /**
   * Backend hook: wait up to \p sliceSeconds for a pending mapping, and report what it found and
   * how it spent the slice. The runtime owns the deadline and the caller's cancellation; this
   * reports only the mapping.
   *
   * @param mappingSlotIndex Slot of the pending mapping.
   * @param sliceSeconds Longest this call may block.
   */
  virtual MapSliceReport onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds);

  /// Backend hook: bytes of a completed mapping. @param mappingSlotIndex Slot of the mapping.
  virtual Result<std::span<const uint8_t>> onMappedBytes(uint32_t mappingSlotIndex) const;

  /// Backend hook: release a mapping. @param mappingSlotIndex Slot of the mapping.
  virtual void onUnmapBuffer(uint32_t mappingSlotIndex);

  /**
   * Backend hook: create a surface for a platform object. Defaults to reporting presentation as
   * unsupported, so a backend that cannot present needs no implementation.
   *
   * @param slotIndex Slot the surface occupies.
   * @param descriptor Label and platform object.
   */
  virtual Status onCreateSurface(uint32_t slotIndex, const SurfaceDescriptor& descriptor);

  /// Backend hook: what a surface supports. @param slotIndex Slot of the surface.
  virtual Result<SurfaceCapabilities> onSurfaceCapabilities(uint32_t slotIndex) const;

  /// Backend hook: apply a configuration. @param slotIndex Slot of the surface.
  /// @param configuration Configuration to apply.
  virtual Status onConfigureSurface(uint32_t slotIndex, const SurfaceConfiguration& configuration);

  /**
   * Backend hook: acquire the next frame's texture and report the surface's state.
   *
   * @param slotIndex Slot of the surface.
   * @param textureSlotIndex Slot the runtime allocated for the acquired texture.
   */
  virtual Result<SurfaceStatus> onAcquireCurrentTexture(uint32_t slotIndex,
                                                        uint32_t textureSlotIndex);

  /// Backend hook: present the acquired texture. @param slotIndex Slot of the surface.
  virtual Result<SurfaceStatus> onPresentSurface(uint32_t slotIndex);

  /// Backend hook: drop the acquired texture without presenting.
  /// @param slotIndex Slot of the surface.
  virtual void onAbandonCurrentTexture(uint32_t slotIndex);

  /**
   * Serial of the last submission that referenced the texture at \p textureSlotIndex, or 0 when
   * nothing has submitted work naming it.
   *
   * For a backend that has to order something against one texture's own work rather than against
   * whatever the device submitted most recently, which is not the same thing as soon as a caller
   * submits anything else between the two.
   *
   * @param textureSlotIndex Slot of the texture.
   */
  uint64_t lastTextureUseSerial(uint32_t textureSlotIndex) const;

  /**
   * Backend hook: release the platform state of a surface that is going away.
   *
   * Runs once per surface, when it is destroyed or when its last handle is dropped, after any
   * frame it still held has been handed back through \ref onAbandonCurrentTexture. The slot is
   * reused by the next surface, so a backend that keeps per-surface state clears it here rather
   * than leaving the next surface to find its predecessor's.
   *
   * @param slotIndex Slot of the surface.
   */
  virtual void onDestroySurface(uint32_t slotIndex);

  /**
   * Backend hook: validated command buffers were submitted as one submission.
   *
   * The span holds at least one buffer and they execute in the order given. The backend owes the
   * caller one completion for the whole span: whatever native objects it splits the work across,
   * \ref completedSerial reaches \p submissionSerial only once all of them have finished, and a
   * backend that reports the submission accepted has accepted all of it.
   *
   * @param submissionSerial Serial assigned to this submission.
   * @param commandBuffers Command buffers of this submission, in execution order.
   */
  virtual Status onSubmit(uint64_t submissionSerial,
                          std::span<const SubmittedCommandBuffer> commandBuffers) = 0;

  /**
   * Backend hook: \ref onSubmit for a submission that must also wait on the device, before any of
   * its work runs, for producer work it names through registrations ordered on the device
   * (\ref SourceOrdering::WaitOnDevice). Every serial in \p waits is one its producer has handed
   * to its native queue, and each backing appears once, with the latest serial it needs.
   *
   * The default passes a submission with no waits to \ref onSubmit and refuses one with waits.
   * Only a backend whose exports report \ref SourceOrdering::WaitOnDevice receives waits, since
   * a registration's producer always belongs to the consumer's own backend, and that backend
   * overrides this.
   *
   * @param submissionSerial Serial assigned to this submission.
   * @param commandBuffers Command buffers of this submission, in execution order.
   * @param waits Producer work the submission waits for on the device; empty when there is none.
   */
  virtual Status onSubmitAfterSources(uint64_t submissionSerial,
                                      std::span<const SubmittedCommandBuffer> commandBuffers,
                                      std::span<const SourceWait> waits);

  /**
   * Backend hook: the bytes the write \ref onWriteTexture just accepted handed the backend's
   * queue, which \ref DeviceObserver::onTextureWritten reports. Asked only when an observer is
   * installed, immediately after \ref onWriteTexture returned success. The default is the
   * caller's whole span; a backend that repacks the rows before uploading reports the repacked
   * size.
   *
   * @param data The caller's span.
   */
  virtual uint64_t onTextureWriteByteCount(std::span<const uint8_t> data) const;

  /// Reports a queue submission the backend made on its own, outside \ref submit, to the
  /// installed observer as a submission of no command buffers and no draws.
  void notifyObserverOfBackendSubmission() const;

private:
  friend class CommandEncoder;

  /**
   * Whether \p bufferSlotIndex currently has a mapping that can still be read. Mappings whose
   * buffer was retired do not count: they name a slot whose occupant is gone.
   *
   * @param bufferSlotIndex Slot of the buffer.
   */
  [[nodiscard]] bool bufferHasOpenMapping(uint32_t bufferSlotIndex) const;

  /**
   * Records what a completed wait observed, so \ref mappedBytes knows whether a wait has seen
   * this mapping complete. Private because readiness is the runtime's own observation: a backend
   * that could set it would be able to declare a mapping readable without one.
   *
   * @param mapping Mapping the wait was for.
   * @param outcome What the wait reported.
   */
  void noteMappingOutcome(const BufferMapping& mapping, MapWaitOutcome outcome);

  /// Validated per-buffer state.
  struct BufferRecord {
    BufferDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-surface state.
  struct SurfaceRecord {
    SurfaceDescriptor descriptor;                    //!< Creation descriptor.
    std::optional<SurfaceConfiguration> configured;  //!< Current configuration, if any.
    /// Texture currently acquired from this surface, invalidated on present, abandon and
    /// reconfiguration. Named by reference because the caller holds the owning handle; releasing
    /// the slot is what makes that handle stale.
    TextureRef acquired;
  };
  /// Validated per-mapping state.
  struct MappingRecord {
    uint32_t bufferSlotIndex = 0;  //!< Slot of the mapped buffer.
    MapMode mode = MapMode::Read;  //!< How the host accesses the range.
    uint64_t offsetBytes = 0;      //!< Byte offset of the mapped range.
    uint64_t byteCount = 0;        //!< Length of the mapped range.
    /// Whether a wait has observed this mapping complete. Reading is refused until it has, so a
    /// caller cannot read a range the GPU may still be writing.
    bool ready = false;
    /// Whether the mapped buffer was destroyed while this mapping was still open. The mapping
    /// outlives the buffer as a handle, but the bytes it named are gone.
    bool bufferRetired = false;
  };
  /// Validated per-texture state.
  struct TextureRecord {
    TextureDescriptor descriptor;  //!< Creation descriptor.
    /// Whether the backing is an allocation this device made and still owns, set at creation
    /// whether or not an observer is installed. \ref destroyTextureBacking reports the end of that
    /// ownership and clears it, so the slot's later recycle reports nothing more. Otherwise the
    /// texture's retirement, which drops this record, carries the value to the slot's recycle,
    /// which reports it, at once or from \ref PendingDestroy after the last submission using the
    /// texture completes. So the end is reported at most once, and only to an observer installed
    /// when it happens. An export can keep the allocation alive after this device's ownership
    /// ends; \ref sharedTextureTailBytes counts those bytes.
    bool ownsAllocation = false;
  };
  /// Validated per-view state. Consumers re-resolve the viewed texture through
  /// \ref resolveViewedTexture on every use, so a view cannot outlive its texture unnoticed.
  struct TextureViewRecord {
    TextureViewDescriptor descriptor;  //!< Creation descriptor.
    ResourceIdentity textureIdentity;  //!< Identity of the viewed texture.
  };
  /// Validated per-sampler state.
  struct SamplerRecord {
    SamplerDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-layout state.
  struct BindGroupLayoutRecord {
    BindGroupLayoutDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-bind-group state.
  struct BindGroupRecord {
    BindGroupDescriptor descriptor;   //!< Creation descriptor.
    ResourceIdentity layoutIdentity;  //!< Identity of the layout this group was created against.
  };
  /// Validated per-pipeline-layout state.
  struct PipelineLayoutRecord {
    PipelineLayoutDescriptor descriptor;               //!< Creation descriptor.
    std::vector<ResourceIdentity> bindGroupLayoutIds;  //!< Layout identities, by group index.
  };
  /// Validated per-shader-module state.
  struct ShaderModuleRecord {
    ShaderModuleDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Buffer range required by the selected stages, independent of shader-module lifetime.
  struct PipelineBufferRequirement {
    uint32_t group = 0;         //!< Required bind group.
    uint32_t binding = 0;       //!< Buffer binding within the group.
    uint64_t minSizeBytes = 0;  //!< Largest requirement across the selected stages.
  };

  /// Validated per-pipeline state used for draw-time compatibility checks.
  struct RenderPipelineRecord {
    RenderPipelineDescriptor descriptor;               //!< Creation descriptor.
    std::vector<ResourceIdentity> bindGroupLayoutIds;  //!< Pipeline layout's group identities.
    std::vector<PipelineBufferRequirement> bufferRequirements;  //!< Validated buffer ranges.
  };
  /// Validated per-pipeline state used for dispatch-time compatibility checks.
  struct ComputePipelineRecord {
    ComputePipelineDescriptor descriptor;              //!< Creation descriptor.
    std::vector<ResourceIdentity> bindGroupLayoutIds;  //!< Pipeline layout's group identities.
    std::vector<PipelineBufferRequirement> bufferRequirements;  //!< Validated buffer ranges.
  };
  /// A finished, not-yet-submitted command buffer.
  struct CommandBufferRecord {
    std::vector<Command> commands;  //!< Validated commands in recording order.
  };

  /// A command buffer a submission has taken out of its slot, with the commands it carried.
  struct ConsumedCommandBuffer {
    uint32_t slotIndex = 0;         //!< Slot the buffer occupied before it was consumed.
    std::vector<Command> commands;  //!< Commands the buffer carried, in recording order.
  };

  /**
   * Takes every command buffer of a submission out of its slot, leaving the caller's handles
   * null, and reports the first identity refusal.
   *
   * Consumption continues past a refusal, so a span refused partway cannot be submitted a second
   * time with the buffers that were accepted the first time already gone.
   *
   * @param commandBuffers Handles to consume, in execution order.
   * @param consumed Receives the buffers whose identities validated, in the same order.
   */
  Status consumeSubmissionCommandBuffers(std::span<CommandBuffer> commandBuffers,
                                         std::vector<ConsumedCommandBuffer>& consumed);

  /**
   * Resolves a handle or handle reference against \p table: null handles and stale generations
   * return \ref GpuErrorType::InvalidHandle, foreign devices return
   * \ref GpuErrorType::DeviceMismatch.
   *
   * @param table Table for the handle's resource type.
   * @param handleLike Handle or \ref HandleRef to resolve.
   * @param resourceName Resource type name for diagnostics.
   */
  template <typename Record, typename HandleLike>
  Result<const Record*> resolve(const details::SlotTable<Record>& table,
                                const HandleLike& handleLike, std::string_view resourceName) const;

  /**
   * Re-resolves the texture a view was created against. Returns
   * \ref GpuErrorType::InvalidHandle if the texture was destroyed, including when its slot was
   * reused by a newer texture, so stale views can never alias another texture.
   *
   * @param viewRecord Resolved record of the view being consumed.
   */
  Result<const TextureRecord*> resolveViewedTexture(const TextureViewRecord& viewRecord) const;

  /// Resource kinds with deferrable backend destruction, indexing the destroy dispatch in
  /// \ref recycleRetiredSlot.
  enum class ResourceKind : uint8_t {
    Buffer,
    Texture,
    TextureView,
    Sampler,
    BindGroupLayout,
    BindGroup,
    PipelineLayout,
    ShaderModule,
    RenderPipeline,
    ComputePipeline,
  };

  /// A destroyed resource whose backend object is awaiting submission completion.
  struct PendingDestroy {
    uint64_t readySerial = 0;  //!< Backend destruction is safe once completedSerial() >= this.
    ResourceKind kind = ResourceKind::Buffer;  //!< Resource kind, for table dispatch.
    uint32_t slotIndex = 0;                    //!< Retired slot index.
    /// Whether recycling the slot ends this device's ownership of a texture allocation it made.
    bool releasesTextureAllocation = false;
  };

  /// Allocates a slot in \p table and mints a handle carrying this device's identity.
  /// @param table Destination table. @param record Validated record to store.
  template <typename Tag, typename Record>
  Handle<Tag> allocateHandle(details::SlotTable<Record>& table, Record&& record);

  /// Shared implementation of the `destroy*` methods: consumes the handle, retires the slot
  /// immediately, and defers the backend release until in-flight submissions complete.
  /// @param table Table for the handle's resource type. @param handle Handle to destroy.
  /// @param kind Resource kind for deferred dispatch.
  template <typename Record, typename Tag>
  Status destroyResource(details::SlotTable<Record>& table, Handle<Tag>&& handle,
                         ResourceKind kind);

  /// Releases the texture \p surface has acquired, if any, so the caller's handle goes stale.
  /// @param surface Already-validated surface handle.
  void releaseAcquiredSurfaceTexture(const Surface& surface);

  /// Same, addressed by slot identity, for the path a dropped handle takes.
  /// @param slotIndex Slot of the surface. @param generation Generation the handle carried.
  void releaseAcquiredSurfaceTextureBySlot(uint32_t slotIndex, uint32_t generation);

  /// Whether \p record still holds a frame the caller could resolve.
  ///
  /// The reference a surface keeps to its frame is not evidence on its own: the caller owns the
  /// handle and may destroy the texture through it, which leaves the reference naming a slot
  /// that no longer holds it. Only a reference whose texture is still live can name a frame the
  /// caller has yet to present or abandon; a dead one means the caller already disposed of it.
  ///
  /// @param record Already-resolved surface record.
  bool hasOutstandingFrame(const SurfaceRecord& record) const;

  /// Whether \p texture is the frame a surface of this device currently has out, which belongs to
  /// that surface rather than to whoever holds the handle.
  /// @param texture Already-validated live texture handle.
  bool namesAcquiredSurfaceFrame(const Texture& texture) const;

  /// Mutable access to an already-validated surface's record, or null if it is not live.
  /// @param surface Already-validated surface handle.
  SurfaceRecord* mutableSurfaceRecord(const Surface& surface);

  /// Whether retiring \p record releases a texture allocation this device owns. Only a texture
  /// record can; every other kind answers false. @param record Record about to be retired.
  template <typename Record>
  static bool ReleasesTextureAllocation(const Record& record) {
    if constexpr (std::is_same_v<Record, TextureRecord>) {
      return record.ownsAllocation;
    } else {
      return false;
    }
  }

  /// Releases the backend object of a retired slot and recycles the slot for reuse.
  /// @param kind Resource kind. @param slotIndex Retired slot index.
  void recycleRetiredSlot(ResourceKind kind, uint32_t slotIndex);

  /// Tells the observer, if any, that this device's ownership of a texture allocation it made
  /// ended, when \p released says it did.
  /// @param released Whether the release just performed ended such an ownership.
  void reportTextureRelease(bool released) const;

  /// Retires a resolved resource: defers the backend release if the resource is referenced by an
  /// incomplete submission, otherwise releases it immediately.
  /// @param kind Resource kind. @param slotIndex Slot index. @param lastUseSerial Serial of the
  /// last submission referencing the resource.
  /// @param releasesTextureAllocation Whether the resource is a texture allocation the device still
  ///   owns, whose ownership ends, and is reported, when the slot is recycled.
  void retireResource(ResourceKind kind, uint32_t slotIndex, uint64_t lastUseSerial,
                      bool releasesTextureAllocation);

  /// RAII destructor path: destroys the resource identified by (\p slotIndex, \p generation) in
  /// \p table if it is still alive; a silent no-op when the identity is stale.
  /// @param table Table for the resource type. @param slotIndex Slot index.
  /// @param generation Generation the handle carried. @param kind Resource kind.
  template <typename Record>
  void releaseFromRaii(details::SlotTable<Record>& table, uint32_t slotIndex, uint32_t generation,
                       ResourceKind kind);

  template <typename Tag>
  friend void details::ReleaseHandleFromRaii(Device& device, uint32_t slotIndex,
                                             uint32_t generation);

  /// One resource referenced by a submission, collected during validation and marked in-use
  /// after the backend accepts the submission.
  struct SubmissionUse {
    ResourceKind kind = ResourceKind::Buffer;  //!< Resource kind, for table dispatch.
    uint32_t slotIndex = 0;                    //!< Resource slot index.
  };

  /// Validates every resource identity referenced by \p commands - including transitive
  /// bind-group entry resources and the textures behind attachment and entry views - and returns
  /// the referenced resources. A destroyed reference fails closed with
  /// \ref GpuErrorType::InvalidHandle.
  /// @param commands Recorded commands.
  Result<std::vector<SubmissionUse>> validateSubmissionResources(
      std::span<const Command> commands) const;

  /**
   * Refuses a submission that uses a buffer with an open mapping.
   *
   * The mapped range aliases the buffer's own storage and its readiness was fixed at the
   * submission it was taken against, so work accepted now would be written underneath a host that
   * still reads the mapping as ready. This covers uses that only read the buffer as well: a
   * reader cannot tell which parts of the range a submission will touch, so the buffer belongs
   * either to the host or to the device and not to both at once.
   *
   * @param uses Resources the submission references.
   */
  Status checkSubmissionMappings(std::span<const SubmissionUse> uses) const;

  /// Resolves one recorded identity against \p table and records it in \p uses, failing closed
  /// with \ref GpuErrorType::InvalidHandle when the slot has been destroyed or reused.
  /// @param table Table owning the resource kind.
  /// @param identity Recorded slot and generation.
  /// @param kind Resource kind stored in \p uses for later table dispatch.
  /// @param resourceName Resource kind name, for the error message.
  /// @param context Description of what referenced the resource, for the error message.
  /// @param uses Accumulator of resources referenced by the submission.
  template <typename Record>
  Result<const Record*> checkSubmissionResource(const details::SlotTable<Record>& table,
                                                const ResourceIdentity& identity, ResourceKind kind,
                                                std::string_view resourceName,
                                                std::string_view context,
                                                std::vector<SubmissionUse>& uses) const;

  /// Resolves a recorded texture view plus the texture behind it: a view of a destroyed texture
  /// must fail even while the view itself is alive.
  /// @param viewIdentity Recorded view slot and generation.
  /// @param context Description of what referenced the view, for the error message.
  /// @param uses Accumulator of resources referenced by the submission.
  Status checkSubmissionTextureView(const ResourceIdentity& viewIdentity, std::string_view context,
                                    std::vector<SubmissionUse>& uses) const;

  /// Resolves every attachment view of a recorded render pass.
  /// @param descriptor Recorded render pass descriptor.
  /// @param uses Accumulator of resources referenced by the submission.
  Status checkSubmissionRenderPass(const RenderPassDescriptor& descriptor,
                                   std::vector<SubmissionUse>& uses) const;

  /// Resolves a recorded bind group, the layout it was created against, and every resource its
  /// entries reference, so a destroyed dependency fails closed even though the group object
  /// itself is alive.
  /// @param groupIdentity Recorded bind group slot and generation.
  /// @param uses Accumulator of resources referenced by the submission.
  Status checkSubmissionBindGroup(const ResourceIdentity& groupIdentity,
                                  std::vector<SubmissionUse>& uses) const;

  /// Validates one generated shader requirement against the pipeline's declared group layout.
  /// @param layout Pipeline layout being used. @param info Generated shader binding facts.
  Status validatePipelineBufferBinding(const PipelineLayoutRecord& layout,
                                       const ShaderBufferBindingInfo& info) const;

  /// Merges one selected entry point's buffer requirements into the pipeline's retained facts.
  /// @param layout Pipeline layout. @param module Shader descriptor carrying generated facts.
  /// @param entryPoint Selected entry point. @param stage Selected shader stage.
  /// @param requirements Destination list, merging shared bindings by their largest minimum.
  Status appendPipelineBufferRequirements(
      const PipelineLayoutRecord& layout, const ShaderModuleDescriptor& module,
      std::string_view entryPoint, ShaderStage stage,
      std::vector<PipelineBufferRequirement>& requirements) const;

  /// Finds the bind group entry matching \p layoutEntry's binding number, failing closed on a
  /// duplicate or missing entry.
  /// @param descriptor Bind group descriptor being validated.
  /// @param layoutEntry Layout entry to match.
  /// @param match Set to the matching entry on success.
  Status findBindGroupEntryForBinding(const BindGroupDescriptor& descriptor,
                                      const BindGroupLayoutEntry& layoutEntry,
                                      const BindGroupEntry*& match) const;

  /// Validates a buffer binding's offset alignment and that its range fits the buffer.
  /// @param entry Bind group entry being validated.
  /// @param bufferBinding Buffer binding carried by \p entry.
  /// @param bufferLabel Label of the bound buffer, for the error message.
  /// @param bufferByteSize Creation size of the bound buffer.
  Status validateBufferBindingRange(const BindGroupEntry& entry, const BufferBinding& bufferBinding,
                                    std::string_view bufferLabel, uint64_t bufferByteSize) const;

  /// Validates a buffer bind group entry against its layout entry: resource kind, usage, and
  /// bound range.
  /// @param layoutEntry Layout entry declaring the binding.
  /// @param entry Bind group entry being validated.
  Status validateBufferBindingEntry(const BindGroupLayoutEntry& layoutEntry,
                                    const BindGroupEntry& entry) const;

  /// Validates a sampled-texture entry: resource kind, live view and texture, usage, and format.
  /// @param layoutEntry Layout entry declaring the sample type.
  /// @param entry Bind group entry being validated.
  Status validateSampledTextureBindingEntry(const BindGroupLayoutEntry& layoutEntry,
                                            const BindGroupEntry& entry) const;

  /// Validates a storage-texture bind group entry: resource kind, live view and texture, the
  /// StorageBinding usage, and that the texture's format matches the one the layout declares.
  /// @param layoutEntry Layout entry declaring the binding.
  /// @param entry Bind group entry being validated.
  Status validateStorageTextureBindingEntry(const BindGroupLayoutEntry& layoutEntry,
                                            const BindGroupEntry& entry) const;

  /// Validates a sampler bind group entry: resource kind and live sampler.
  /// @param entry Bind group entry being validated.
  Status validateSamplerBindingEntry(const BindGroupEntry& entry) const;

  /// One texture binding of a bind group: the binding index and the texture its view names.
  struct BoundTextureBinding {
    uint32_t binding = 0;              //!< Shader binding index.
    ResourceIdentity textureIdentity;  //!< Identity of the texture behind the bound view.
  };

  /// Collects sampled and storage-write texture bindings in one walk of \p layoutEntries,
  /// appending to caller-owned storage. The draw and dispatch path calls this once per bound
  /// group, so it allocates nothing while the outputs stay inside their inline capacity, which
  /// bounds the total across every bound group rather than the largest single one. Two passes
  /// over the layout and two returned vectors showed up as avoidable per-draw work.
  /// @param descriptor Bind group descriptor being validated.
  /// @param layoutEntries Layout the group was created against.
  /// @param sampledOut Receives every sampled-texture binding that resolves to a live view.
  /// @param storageOut Receives every storage-write binding that resolves to a live view.
  void collectBoundTextures(const BindGroupDescriptor& descriptor,
                            const std::vector<BindGroupLayoutEntry>& layoutEntries,
                            SmallVector<BoundTextureBinding, kMaxBindings>& sampledOut,
                            SmallVector<BoundTextureBinding, kMaxBindings>& storageOut) const;

  /// Rejects a bind group that names one texture through both a sampled and a storage-write
  /// binding: the two declare different layouts for one image, so neither backend transition can
  /// satisfy both at dispatch.
  /// @param descriptor Bind group descriptor being validated.
  /// @param layoutEntries Entries of the layout it was created against.
  Status validateNoTextureAliasing(const BindGroupDescriptor& descriptor,
                                   const std::vector<BindGroupLayoutEntry>& layoutEntries) const;

  /// Validates one bind group entry against the layout entry declaring its binding.
  /// @param layoutEntry Layout entry declaring the binding.
  /// @param entry Bind group entry being validated.
  Status validateBindGroupEntryForLayout(const BindGroupLayoutEntry& layoutEntry,
                                         const BindGroupEntry& entry) const;

  /// Resolves the texture and buffer a recorded texture-to-buffer copy references.
  /// @param command Recorded copy command.
  /// @param uses Accumulator of resources referenced by the submission.
  Status checkSubmissionCopyToBuffer(const CopyTextureToBufferCommand& command,
                                     std::vector<SubmissionUse>& uses) const;

  /// Resolves both textures a recorded texture-to-texture copy references.
  /// @param command Recorded copy command.
  /// @param uses Accumulator of resources referenced by the submission.
  Status checkSubmissionCopyToTexture(const CopyTextureToTextureCommand& command,
                                      std::vector<SubmissionUse>& uses) const;

  /// Resolves every resource one recorded command references.
  /// @param command Recorded command.
  /// @param uses Accumulator of resources referenced by the submission.
  Status checkSubmissionCommand(const Command& command, std::vector<SubmissionUse>& uses) const;

  /// Marks one resource as used by \p submissionSerial in its own slot table.
  /// @param kind Resource kind. @param slotIndex Resource slot index.
  /// @param submissionSerial Serial of the accepted submission.
  void markResourceUsed(ResourceKind kind, uint32_t slotIndex, uint64_t submissionSerial);

  /// Marks every collected resource as used by \p submissionSerial, deferring its backend
  /// destruction until that submission completes.
  /// @param uses Resources collected by \ref validateSubmissionResources.
  /// @param submissionSerial Serial assigned to the accepted submission.
  void markSubmissionUses(std::span<const SubmissionUse> uses, uint64_t submissionSerial);

  /// Registers a finished command stream from an encoder and returns its handle.
  /// @param commands Validated commands in recording order.
  CommandBuffer registerCommandBuffer(std::vector<Command>&& commands);

  /// What this device holds for a texture slot that names another device's texture.
  struct TextureRegistration {
    /// This registration's holder of the producer's share.
    std::shared_ptr<const details::TextureShareLease> lease;
    /// Producer serial whose completion consumer work naming this texture must follow.
    uint64_t orderAfterSerial = 0;
  };
  // Lives in a growing vector, so reallocation must move it without throwing.
  static_assert(std::is_nothrow_move_constructible_v<TextureRegistration>);

  /// Releases a texture's allocation at once, or, while an export or another device's
  /// registration still holds it, when the last holder lets go. @param slotIndex Texture slot.
  void releaseTextureBackingOrDefer(uint32_t slotIndex);

  /// Refuses exporting a texture of a lost device or a registration.
  /// @param texture Already-resolved texture. @param descriptor Its record, for the message.
  Status checkTextureExportable(const Texture& texture, const TextureDescriptor& descriptor) const;

  /// Refuses exporting a frame a surface has out unless its registrations share this device's
  /// queue. @param texture Already-resolved texture. @param descriptor Its record, for the
  /// message. @param created The backend's export of it, or why there is none.
  Status checkSurfaceFrameExport(
      const Texture& texture, const TextureDescriptor& descriptor,
      const Result<std::shared_ptr<details::TextureShare>>& created) const;

  /// Asks the backend to export a texture and records the share every token will hold.
  /// @param slotIndex Exportable texture slot. @param descriptor Its record.
  Result<std::shared_ptr<details::TextureShare>> createTextureShare(
      uint32_t slotIndex, const TextureDescriptor& descriptor);

  /// Refuses an export this device cannot name: another backend family or native device, or a
  /// backend without cross-device naming. @param share Share the export holds.
  Status checkRegistrationIdentity(const details::TextureShare& share) const;

  /// Refuses registering an export of this device, of a lost or released texture, of another
  /// backend or native device, or of a texture with a write still queued.
  /// @param share Share the export holds.
  Status checkRegistrationSource(const details::TextureShare& share) const;

  /// Whether work naming a registration may be submitted: true once the producer work it follows
  /// completed, or, for a registration ordered on the device, once that work was handed to the
  /// producer's queue; false once either device is lost or the producer failed (declaring that
  /// failure); and nothing while it is still pending. @param entry Registration to check.
  std::optional<bool> textureSourceState(const TextureRegistration& entry) const;

  /// Whether work naming a registration of \p share waits on the device for the producer: the
  /// producer's backend orders it there (\ref SourceOrdering::WaitOnDevice), and the producer
  /// shares this device's loss condition, so a failure or loss that ends the wait is this
  /// device's loss too. @param share Share of the registered texture.
  bool ordersOnDevice(const details::TextureShare& share) const;

  /// The share of an exported texture of this device, or null. @param slotIndex Texture slot.
  details::TextureShare* textureShareOf(uint32_t slotIndex) const;

  /// The registration in a texture slot, or null. @param slotIndex Texture slot.
  const TextureRegistration* textureRegistrationOf(uint32_t slotIndex) const;

  /// Refuses one registration a submission names when either device is lost or failed, or when
  /// its producer work has not completed and cannot be waited for on the device; records the
  /// device-side wait when it can be.
  /// @param entry Registration. @param slotIndex Its slot.
  /// @param waits Device-side waits of the submission, each backing once with its latest serial.
  Status checkTextureSourceReady(const TextureRegistration& entry, uint32_t slotIndex,
                                 std::vector<SourceWait>& waits) const;

  /// Refuses a submission naming a registration whose producer work has not completed and cannot
  /// be waited for on the device, or whose producer or this device is lost, and collects the
  /// device-side waits the submission needs. @param uses Resources the submission references.
  /// @param waits Receives the device-side waits.
  Status checkSubmissionTextureSources(std::span<const SubmissionUse> uses,
                                       std::vector<SourceWait>& waits) const;

  /// Records an accepted submission in the shares of the exported textures it referenced, and
  /// marks queued writes to exported textures as carried by it.
  /// @param uses Resources the submission referenced. @param submissionSerial Accepted serial.
  void noteSubmittedTextureShares(std::span<const SubmissionUse> uses, uint64_t submissionSerial);

  /// Records an accepted write to an exported texture that is waiting for the next submission.
  /// @param slotIndex Written texture slot.
  void noteTextureWriteForShares(uint32_t slotIndex);

  /// The texture slot's handle was released: the producer stops holding its share.
  /// @param slotIndex Retired texture slot.
  void releaseTextureShare(uint32_t slotIndex);

  /// The texture slot is being recycled: a registration in it stops holding the producer's share.
  /// @param slotIndex Recycled texture slot.
  void releaseTextureRegistration(uint32_t slotIndex);

  /// Returns the next process-unique device id.
  static uint64_t NextDeviceId();

  uint64_t deviceId_ = 0;
  uint64_t lastSubmittedSerial_ = 0;

  /// Notified of accepted operations; see \ref installObserver. Non-owning.
  DeviceObserver* observer_ = nullptr;

  /// Sticky loss condition of the backend root, shared with every other device over it. Created
  /// here so a device whose backend never shares one still has somewhere to publish a loss.
  std::shared_ptr<DeviceLostState> lostState_ = std::make_shared<DeviceLostState>();

  /// Device-alive token shared with every minted handle: `~Device` releases it, so a handle
  /// destroyed after its device skips the RAII release.
  std::shared_ptr<Device*> aliveToken_;

  /// Destroyed resources awaiting backend release, in destruction order (drained FIFO by
  /// \ref poll so backend release order is deterministic).
  std::vector<PendingDestroy> pendingDestroys_;

  details::SlotTable<BufferRecord> buffers_;
  details::SlotTable<MappingRecord> bufferMappings_;
  details::SlotTable<SurfaceRecord> surfaces_;
  details::SlotTable<TextureRecord> textures_;
  details::SlotTable<TextureViewRecord> textureViews_;
  details::SlotTable<SamplerRecord> samplers_;
  details::SlotTable<BindGroupLayoutRecord> bindGroupLayouts_;
  details::SlotTable<BindGroupRecord> bindGroups_;
  details::SlotTable<PipelineLayoutRecord> pipelineLayouts_;
  details::SlotTable<ShaderModuleRecord> shaderModules_;
  details::SlotTable<RenderPipelineRecord> renderPipelines_;
  details::SlotTable<ComputePipelineRecord> computePipelines_;
  details::SlotTable<CommandBufferRecord> commandBuffers_;

  /// Share of each exported texture of this device, by texture slot; null for a texture never
  /// exported. Cleared when the producer releases the handle.
  std::vector<std::shared_ptr<details::TextureShare>> textureShares_;
  /// Registration held by each texture slot that names another device's texture. Cleared when
  /// the slot is recycled, so the producer's allocation outlives this device's last use of it.
  std::vector<std::optional<TextureRegistration>> textureRegistrations_;
  /// Texture slots of exported textures with a write waiting for the next submission.
  std::vector<uint32_t> pendingSharedTextureWrites_;
  /// See \ref sharedTextureTailBytes. Shared with every share this device exports, so a holder
  /// that outlives this device still settles its bytes.
  std::shared_ptr<std::atomic<uint64_t>> sharedTextureTailBytes_ =
      std::make_shared<std::atomic<uint64_t>>(0);
};

template <typename Record, typename HandleLike>
Result<const Record*> Device::resolve(const details::SlotTable<Record>& table,
                                      const HandleLike& handleLike,
                                      std::string_view resourceName) const {
  if (!handleLike.isValid()) {
    return GpuError{
        GpuErrorType::InvalidHandle,
        std::format("{} handle is null (default-constructed or moved-from)", resourceName)};
  }
  if (handleLike.deviceId() != deviceId_) {
    return GpuError{GpuErrorType::DeviceMismatch,
                    std::format("{} handle belongs to device {} but was used with device {}",
                                resourceName, handleLike.deviceId(), deviceId_)};
  }
  const Record* record = table.find(handleLike.slotIndex(), handleLike.generation());
  if (record == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("{} handle (slot {}) is stale; the resource was destroyed",
                                resourceName, handleLike.slotIndex())};
  }
  return record;
}

}  // namespace donner::gpu
