#pragma once
/// @file
/// \c donner::gpu::CommandEncoder, \c donner::gpu::RenderPassEncoder, and
/// \c donner::gpu::ComputePassEncoder - fail-closed command recording.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "donner/gpu/Commands.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/GpuResult.h"

namespace donner::gpu {

class CommandEncoder;

/// Which kind of pass a \ref CommandEncoder currently has open.
enum class PassKind : uint8_t {
  None,     //!< No pass is active; copies and pass begins are allowed.
  Render,   //!< A render pass is active.
  Compute,  //!< A compute pass is active.
};

/**
 * Encodes commands inside an active render pass. Obtained from
 * \ref CommandEncoder::beginRenderPass; owned by the encoder and valid until the encoder is
 * destroyed (operations after \ref end fail with \ref GpuErrorType::InvalidState).
 */
class RenderPassEncoder {
public:
  RenderPassEncoder(const RenderPassEncoder&) = delete;
  RenderPassEncoder& operator=(const RenderPassEncoder&) = delete;
  RenderPassEncoder(RenderPassEncoder&&) = delete;
  RenderPassEncoder& operator=(RenderPassEncoder&&) = delete;

  /**
   * Sets the active render pipeline.
   *
   * @param pipeline Pipeline to bind; must be a live handle of the encoder's device.
   */
  Status setPipeline(const RenderPipeline& pipeline);

  /**
   * Binds \p bindGroup at \p index. Compatibility with the active pipeline's layout is checked
   * at \ref draw time, so bind groups may be set before the pipeline.
   *
   * @param index Bind group index; must be below \ref kMaxBindGroups.
   * @param bindGroup Bind group to bind; must be a live handle of the encoder's device.
   */
  Status setBindGroup(uint32_t index, const BindGroup& bindGroup);

  /**
   * Binds \p buffer as the vertex buffer for \p slot.
   *
   * @param slot Vertex buffer slot; must be below \ref kMaxVertexBuffers.
   * @param buffer Buffer to bind; needs \ref BufferUsage::Vertex.
   * @param offsetBytes Byte offset of the first element; must be within the buffer.
   */
  Status setVertexBuffer(uint32_t slot, const Buffer& buffer, uint64_t offsetBytes = 0);

  /**
   * Sets the scissor rectangle. The rectangle must fit inside the pass attachment extent.
   *
   * @param x Left edge in pixels.
   * @param y Top edge in pixels.
   * @param width Width in pixels.
   * @param height Height in pixels.
   */
  Status setScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

  /**
   * Sets the viewport. All values must be finite, with positive extent and
   * `0 <= minDepth <= maxDepth <= 1`.
   *
   * @param x Left edge in pixels.
   * @param y Top edge in pixels.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param minDepth Minimum depth of the viewport range.
   * @param maxDepth Maximum depth of the viewport range.
   */
  Status setViewport(float x, float y, float width, float height, float minDepth, float maxDepth);

  /**
   * Records a draw. Fails closed unless a pipeline is set, every vertex buffer slot the pipeline
   * declares is bound with enough bytes for the drawn range (checked arithmetic), and every bind
   * group index the pipeline layout declares holds a bind group created against the same layout.
   *
   * @param vertexCount Number of vertices.
   * @param instanceCount Number of instances.
   * @param firstVertex First vertex index.
   * @param firstInstance First instance index.
   */
  Status draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0,
              uint32_t firstInstance = 0);

  /// Ends the render pass. Further pass operations fail with
  /// \ref GpuErrorType::InvalidState until a new pass begins.
  Status end();

private:
  friend class CommandEncoder;

  /// Constructs the pass encoder owned by \p encoder.
  /// @param encoder Owning command encoder.
  explicit RenderPassEncoder(CommandEncoder& encoder) : encoder_(&encoder) {}

  CommandEncoder* encoder_;
};

/**
 * Encodes commands inside an active compute pass. Obtained from
 * \ref CommandEncoder::beginComputePass; owned by the encoder and valid until the encoder is
 * destroyed (operations after \ref end fail with \ref GpuErrorType::InvalidState).
 */
class ComputePassEncoder {
public:
  ComputePassEncoder(const ComputePassEncoder&) = delete;
  ComputePassEncoder& operator=(const ComputePassEncoder&) = delete;
  ComputePassEncoder(ComputePassEncoder&&) = delete;
  ComputePassEncoder& operator=(ComputePassEncoder&&) = delete;

  /**
   * Sets the active compute pipeline.
   *
   * @param pipeline Pipeline to bind; must be a live handle of the encoder's device.
   */
  Status setPipeline(const ComputePipeline& pipeline);

  /**
   * Binds \p bindGroup at \p index. Compatibility with the active pipeline's layout is checked
   * at \ref dispatchWorkgroups time, so bind groups may be set before the pipeline.
   *
   * @param index Bind group index; must be below \ref kMaxBindGroups.
   * @param bindGroup Bind group to bind; must be a live handle of the encoder's device.
   */
  Status setBindGroup(uint32_t index, const BindGroup& bindGroup);

  /**
   * Records a dispatch. Fails closed unless a pipeline is set, every bind group index the
   * pipeline layout declares holds a bind group created against the same layout, and every
   * workgroup count is nonzero and within \ref kMaxComputeWorkgroupsPerDimension.
   *
   * @param workgroupCountX Workgroups along X.
   * @param workgroupCountY Workgroups along Y.
   * @param workgroupCountZ Workgroups along Z.
   */
  Status dispatchWorkgroups(uint32_t workgroupCountX, uint32_t workgroupCountY = 1,
                            uint32_t workgroupCountZ = 1);

  /// Ends the compute pass. Further pass operations fail with
  /// \ref GpuErrorType::InvalidState until a new pass begins.
  Status end();

private:
  friend class CommandEncoder;

  /// Constructs the pass encoder owned by \p encoder.
  /// @param encoder Owning command encoder.
  explicit ComputePassEncoder(CommandEncoder& encoder) : encoder_(&encoder) {}

  CommandEncoder* encoder_;
};

/**
 * Records and validates GPU commands for one command buffer.
 *
 * The encoder is a fail-closed state machine: every operation validates handle liveness, device
 * identity, usage flags, bounds, and pass state before recording. The first error latches and
 * poisons the encoder - every subsequent operation, including \ref finish, returns that first
 * error - so a failure cannot be silently skipped and the root cause is always the reported
 * error. Obtain encoders via `Device::createCommandEncoder`; the encoder must not outlive its
 * device.
 *
 * Commands record full (slot, generation) resource identities, and `Device::submit` re-validates
 * every one, so destroying a resource between recording and submission fails closed at submit.
 */
class CommandEncoder {
public:
  CommandEncoder(const CommandEncoder&) = delete;
  CommandEncoder& operator=(const CommandEncoder&) = delete;
  CommandEncoder(CommandEncoder&&) = delete;
  CommandEncoder& operator=(CommandEncoder&&) = delete;
  /// Destructor. Discards recorded commands unless \ref finish transferred them.
  ~CommandEncoder() = default;

  /**
   * Begins a render pass and returns its pass encoder. Fails with
   * \ref GpuErrorType::InvalidState if a pass is already active. Attachment views must be live,
   * share one extent, and their textures need \ref TextureUsage::RenderAttachment.
   *
   * The returned pointer is owned by this encoder and remains valid until the encoder is
   * destroyed.
   *
   * @param descriptor Validated render pass descriptor.
   */
  Result<RenderPassEncoder*> beginRenderPass(const RenderPassDescriptor& descriptor);

  /**
   * Begins a compute pass and returns its pass encoder. Fails with
   * \ref GpuErrorType::InvalidState if a pass is already active.
   *
   * The returned pointer is owned by this encoder and remains valid until the encoder is
   * destroyed.
   *
   * @param descriptor Validated compute pass descriptor.
   */
  Result<ComputePassEncoder*> beginComputePass(const ComputePassDescriptor& descriptor);

  /**
   * Records a texture-to-buffer copy (readback staging). Not allowed inside a render pass.
   * \p destinationLayout must be 256-aligned and cover \p copySize, \p copySize must fit in the
   * source texture, and the described rows must fit in \p destination (checked arithmetic).
   *
   * @param source Source texture; needs \ref TextureUsage::CopySrc.
   * @param destination Destination buffer; needs \ref BufferUsage::CopyDst.
   * @param destinationLayout Row layout in the destination buffer.
   * @param copySize Copy extent in texels.
   */
  Status copyTextureToBuffer(const TexelCopyTextureInfo& source, const Buffer& destination,
                             const TexelCopyBufferLayout& destinationLayout,
                             const Extent2d& copySize);

  /**
   * Records a texture-to-texture copy starting at texel (0, 0) of both textures (the RHI's
   * whole-rect copy convention). Not allowed inside a render pass. Both textures must be live
   * handles of this device and share one \ref TextureFormat, \p source needs
   * \ref TextureUsage::CopySrc, \p destination needs \ref TextureUsage::CopyDst, and
   * \p copySize must be nonzero and fit inside both texture extents.
   *
   * @param source Source texture; needs \ref TextureUsage::CopySrc.
   * @param destination Destination texture; needs \ref TextureUsage::CopyDst.
   * @param copySize Copy extent in texels.
   */
  Status copyTextureToTexture(const Texture& source, const Texture& destination,
                              const Extent2d& copySize);

  /**
   * Finishes recording and registers the command buffer with the device. Fails with the first
   * recorded error if any operation failed, or with \ref GpuErrorType::InvalidState if a pass is
   * still active or the encoder already finished.
   */
  Result<CommandBuffer> finish();

private:
  friend class Device;
  friend class RenderPassEncoder;
  friend class ComputePassEncoder;

  /// Constructs an encoder recording against \p device.
  /// @param device Owning device; must outlive the encoder.
  explicit CommandEncoder(Device& device)
      : device_(&device), passEncoder_(*this), computePassEncoder_(*this) {}

  /// Draw-time validation state for the active pipeline.
  struct BoundPipeline {
    std::vector<VertexBufferLayout> vertexBuffers;     //!< Declared vertex layouts.
    std::vector<ResourceIdentity> bindGroupLayoutIds;  //!< Required group layouts.
  };
  /// Draw-time validation state for one bound vertex buffer slot.
  struct BoundVertexBuffer {
    uint32_t bufferSlot = 0;      //!< Buffer slot index.
    uint64_t bytesAvailable = 0;  //!< Bytes from the bound offset to the end of the buffer.
  };
  /// Draw-time validation state for one bound bind group index.
  struct BoundBindGroup {
    uint32_t bindGroupSlot = 0;       //!< Bind group slot index.
    ResourceIdentity layoutIdentity;  //!< Layout the group was created against.
  };

  /// Latches \p error (first error wins, poisoning the encoder) and returns the latched error.
  /// @param error Error to latch.
  Status fail(GpuError&& error);

  /// Returns the poisoned/finished-state error common to all operations, if any.
  /// @param requiredPass Pass kind the operation needs open, or \ref PassKind::None when the
  ///   operation only requires a usable encoder.
  std::optional<GpuError> checkRecordable(PassKind requiredPass);

  /// Verifies that every bind group index the bound pipeline layout declares holds a group
  /// created against the same layout. Shared by draw and dispatch.
  /// @param operation Operation name for diagnostics, e.g. `"draw"`.
  Status validateBoundBindGroups(std::string_view operation);

  /// Resets the per-pass binding state a begin or end transitions through.
  void resetPassBindings();

  /// Re-resolves the resource one bind group entry references, failing closed when it was
  /// destroyed after the group was created.
  /// @param entry Entry to re-resolve.
  Status revalidateBindGroupEntry(const BindGroupEntry& entry);

  /// Validates that a texture-to-texture copy's operands are distinct, share a format, and carry
  /// the CopySrc / CopyDst usages.
  /// @param source Source texture handle.
  /// @param destination Destination texture handle.
  /// @param sourceDescriptor Descriptor the source was created with.
  /// @param destinationDescriptor Descriptor the destination was created with.
  Status validateCopyTexturePair(const Texture& source, const Texture& destination,
                                 const TextureDescriptor& sourceDescriptor,
                                 const TextureDescriptor& destinationDescriptor);

  /// Validates that a texture-to-texture copy's extent is non-degenerate and fits both operands.
  /// @param copySize Copy extent in texels.
  /// @param sourceDescriptor Descriptor the source was created with.
  /// @param destinationDescriptor Descriptor the destination was created with.
  Status validateCopyExtent(const Extent2d& copySize, const TextureDescriptor& sourceDescriptor,
                            const TextureDescriptor& destinationDescriptor);

  // RenderPassEncoder forwards to these.
  Status passSetPipeline(const RenderPipeline& pipeline);
  Status passSetBindGroup(uint32_t index, const BindGroup& bindGroup);
  Status passSetVertexBuffer(uint32_t slot, const Buffer& buffer, uint64_t offsetBytes);
  Status passSetScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  Status passSetViewport(float x, float y, float width, float height, float minDepth,
                         float maxDepth);
  Status passDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                  uint32_t firstInstance);
  Status passEnd();

  // ComputePassEncoder forwards to these.
  Status computePassSetPipeline(const ComputePipeline& pipeline);
  Status computePassDispatch(uint32_t workgroupCountX, uint32_t workgroupCountY,
                             uint32_t workgroupCountZ);
  Status computePassEnd();

  Device* device_;
  RenderPassEncoder passEncoder_;
  ComputePassEncoder computePassEncoder_;
  std::vector<Command> commands_;
  std::optional<GpuError> firstError_;
  PassKind activePass_ = PassKind::None;
  bool finished_ = false;

  Extent2d passExtent_;
  std::vector<TextureFormat> passAttachmentFormats_;
  std::optional<BoundPipeline> currentPipeline_;
  std::array<std::optional<BoundVertexBuffer>, kMaxVertexBuffers> boundVertexBuffers_;
  std::array<std::optional<BoundBindGroup>, kMaxBindGroups> boundBindGroups_;
};

}  // namespace donner::gpu
