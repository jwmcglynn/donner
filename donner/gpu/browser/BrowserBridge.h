#pragma once
/// @file
/// \c donner::gpu::browser::BrowserBridge - the boundary between the runtime and the browser's
/// GPU service.
///
/// Everything below the runtime's validation and above the browser's own API passes through this
/// interface. It is deliberately narrow and flat: objects are named by \ref BrowserObjectId, every
/// enumerated value arrives as a \ref BrowserWireCodes.h code, and every payload is a span of
/// bytes the browser side copies out. Nothing that could name host memory - a pointer, a native
/// handle, a callback address - crosses it.
///
/// Recorded commands are replayed one call at a time rather than handed over as an encoded
/// stream. That keeps the browser side free of a decoder: every operation it performs is one it
/// was called for, with arguments the runtime already validated, so there is no length, opcode or
/// arity arithmetic on the far side to get wrong.
///
/// The interface is abstract so the runtime's behavior can be tested without a browser. The
/// production implementation calls the browser; the test implementation records what it was
/// asked to do.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/browser/BrowserObjectTable.h"

namespace donner::gpu::browser {

/**
 * What the browser side reports for one bridge call.
 *
 * The refusals are separated because they mean different things about what went wrong. An
 * identifier the browser side has never heard of, one that names a live object of another kind,
 * and a call from a context that does not own the device are three distinct failures of the
 * caller; a lost device and a browser-side rejection are failures of the environment.
 */
enum class BridgeStatus : uint8_t {
  Success,          //!< The browser side performed the operation.
  UnknownObject,    //!< An identifier named no live browser object.
  WrongObjectKind,  //!< An identifier named a live object of a different kind.
  NotOwner,         //!< The calling context does not own the browser device.
  DeviceLost,       //!< The device was lost, so the operation can never be performed.
  Failed,           //!< The browser rejected the operation.
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BridgeStatus value);

/**
 * Progress of an asynchronous request for a browser GPU device.
 *
 * A browser hands over a device through promises, so the request has a lifetime of its own rather
 * than being an outcome the constructor can report. The two failures are separated because they
 * call for different responses: a browser with no GPU service at all is \ref Unavailable and the
 * caller falls back, while a service that refused this request is \ref Failed and the reason
 * says why.
 */
enum class BrowserDeviceRequestState : uint8_t {
  Pending,      //!< The browser has not settled the request yet.
  Ready,        //!< A device is available.
  Unavailable,  //!< The browser exposes no GPU service.
  Failed,       //!< The browser refused to supply a device.
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BrowserDeviceRequestState value);

/// One entry of a bind group layout, with its enumerated values already encoded.
struct BrowserBindGroupLayoutEntry {
  uint32_t binding = 0;               //!< Shader binding index.
  uint32_t visibilityBits = 0;        //!< Encoded \ref ShaderStage mask.
  uint32_t bindingTypeCode = 0;       //!< Encoded \ref BindingType.
  uint32_t storageTextureFormat = 0;  //!< Encoded \ref TextureFormat of a storage-texture binding.
};

/// Which kind of resource a \ref BrowserBindGroupEntry binds.
enum class BrowserBindingResource : uint8_t {
  Buffer,       //!< A range of a buffer.
  TextureView,  //!< A texture view.
  Sampler,      //!< A sampler.
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BrowserBindingResource value);

/// One entry of a bind group, naming the bound object by identifier.
struct BrowserBindGroupEntry {
  uint32_t binding = 0;  //!< Shader binding index.
  /// Which resource kind \ref resourceId names.
  BrowserBindingResource resource = BrowserBindingResource::Buffer;
  BrowserObjectId resourceId = kNoBrowserObject;  //!< Bound object.
  uint64_t offsetBytes = 0;  //!< Byte offset of a bound buffer range; zero otherwise.
  uint64_t sizeBytes = 0;    //!< Byte size of a bound buffer range; zero otherwise.
};

/// One vertex attribute of a render pipeline, with its format already encoded.
struct BrowserVertexAttribute {
  uint32_t formatCode = 0;      //!< Encoded \ref VertexFormat.
  uint32_t offsetBytes = 0;     //!< Byte offset within one element.
  uint32_t shaderLocation = 0;  //!< Location index in the shader.
};

/// One vertex buffer slot of a render pipeline.
struct BrowserVertexBufferLayout {
  uint32_t strideBytes = 0;                        //!< Bytes per element.
  uint32_t stepModeCode = 0;                       //!< Encoded \ref VertexStepMode.
  std::vector<BrowserVertexAttribute> attributes;  //!< Attributes in this slot.
};

/// One blend term of a color target, with its factors and operation already encoded.
struct BrowserBlendComponent {
  uint32_t srcFactorCode = 0;  //!< Encoded source \ref BlendFactor.
  uint32_t dstFactorCode = 0;  //!< Encoded destination \ref BlendFactor.
  uint32_t operationCode = 0;  //!< Encoded \ref BlendOperation.
};

/// One color target of a render pipeline.
struct BrowserColorTarget {
  uint32_t formatCode = 0;           //!< Encoded \ref TextureFormat.
  bool blendEnabled = false;         //!< Whether \ref colorBlend and \ref alphaBlend apply.
  BrowserBlendComponent colorBlend;  //!< RGB blend term.
  BrowserBlendComponent alphaBlend;  //!< Alpha blend term.
  uint32_t writeMaskBits = 0;        //!< Encoded \ref ColorWriteMask.
};

/// A render pipeline, with every referenced object named by identifier.
struct BrowserRenderPipelineRequest {
  BrowserObjectId layoutId = kNoBrowserObject;           //!< Pipeline layout.
  BrowserObjectId vertexModuleId = kNoBrowserObject;     //!< Module holding the vertex entry point.
  RcString vertexEntryPoint;                             //!< Vertex entry point name.
  std::vector<BrowserVertexBufferLayout> vertexBuffers;  //!< Vertex buffer layouts, by slot.
  BrowserObjectId fragmentModuleId = kNoBrowserObject;   //!< Module holding the fragment entry
                                                         //!< point.
  RcString fragmentEntryPoint;                           //!< Fragment entry point name.
  std::vector<BrowserColorTarget> colorTargets;          //!< Color targets.
  uint32_t topologyCode = 0;                             //!< Encoded \ref PrimitiveTopology.
  uint32_t cullModeCode = 0;                             //!< Encoded \ref CullMode.
};

/// A compute pipeline, with every referenced object named by identifier.
struct BrowserComputePipelineRequest {
  BrowserObjectId layoutId = kNoBrowserObject;  //!< Pipeline layout.
  BrowserObjectId moduleId = kNoBrowserObject;  //!< Module holding the compute entry point.
  RcString entryPoint;                          //!< Compute entry point name.
};

/// One color attachment of a render pass, naming its view by identifier.
struct BrowserColorAttachment {
  BrowserObjectId viewId = kNoBrowserObject;  //!< Attachment view.
  uint32_t loadOpCode = 0;                    //!< Encoded \ref LoadOp.
  uint32_t storeOpCode = 0;                   //!< Encoded \ref StoreOp.
  std::array<double, 4> clearColor = {};      //!< Premultiplied RGBA clear color.
};

/// A rectangular region of texels, for the copy operations.
struct BrowserCopyRegion {
  uint32_t sourceX = 0;       //!< Column of the first source texel.
  uint32_t sourceY = 0;       //!< Row of the first source texel.
  uint32_t destinationX = 0;  //!< Column of the first destination texel.
  uint32_t destinationY = 0;  //!< Row of the first destination texel.
  uint32_t width = 0;         //!< Width of the copied region in texels.
  uint32_t height = 0;        //!< Height of the copied region in texels.
};

/// The byte layout of texel rows in a copy, already flattened to scalars.
struct BrowserTexelLayout {
  uint64_t offsetBytes = 0;   //!< Byte offset of the first row.
  uint32_t bytesPerRow = 0;   //!< Bytes between row starts.
  uint32_t rowsPerImage = 0;  //!< Rows allotted to the image.
};

/// What a browser surface reports it supports.
struct BrowserSurfaceCapabilities {
  std::vector<uint32_t> formatCodes;       //!< Encoded \ref TextureFormat values.
  uint32_t usageBits = 0;                  //!< Encoded \ref TextureUsage mask.
  std::vector<uint32_t> presentModeCodes;  //!< Encoded \ref PresentMode values.
  std::vector<uint32_t> alphaModeCodes;    //!< Encoded \ref SurfaceAlphaMode values.
};

/**
 * The browser's GPU service, as the runtime uses it.
 *
 * One instance stands for one browser device. Implementations perform no validation of their own
 * beyond the identifier, kind and ownership checks each call documents: everything else arrives
 * already validated by \ref donner::gpu::Device.
 */
class BrowserBridge {
public:
  /// Destructor; releases the browser device and every object still registered with it.
  virtual ~BrowserBridge();

  BrowserBridge(const BrowserBridge&) = delete;
  BrowserBridge& operator=(const BrowserBridge&) = delete;
  BrowserBridge(BrowserBridge&&) = delete;
  BrowserBridge& operator=(BrowserBridge&&) = delete;

  // Device acquisition. A browser supplies a device asynchronously, so the request is begun and
  // then polled; nothing else on this interface may be called before the request is Ready.

  /// Asks the browser for a GPU device. Begins the request; it settles asynchronously.
  virtual BridgeStatus beginDeviceRequest() = 0;

  /// How far the request begun by \ref beginDeviceRequest has progressed.
  virtual BrowserDeviceRequestState deviceRequestState() const = 0;

  /// Why the request failed, when \ref deviceRequestState reports a failure. Empty otherwise.
  virtual RcString deviceRequestError() const = 0;

  // Device state.

  /// Whether the calling context owns the browser device. A browser device belongs to the worker
  /// that obtained it, and the objects made from it are only usable there.
  virtual bool ownsDevice() const = 0;

  /// Whether the browser has reported the device lost. Once true it never becomes false again.
  virtual bool isDeviceLost() const = 0;

  /// What the browser said when it reported the device lost. Empty while the device is alive.
  virtual RcString deviceLostReason() const = 0;

  /// Serial of the most recent submission the browser has reported finished (0 if none).
  virtual uint64_t completedSerial() const = 0;

  // Resource creation. The caller mints the identifier; the browser side registers the object it
  // creates under that identifier and refuses one that is already in use.

  /// Creates a buffer. @param id Identifier to register it under. @param byteSize Size in bytes.
  /// @param usageBits Encoded \ref BufferUsage mask.
  virtual BridgeStatus createBuffer(BrowserObjectId id, uint64_t byteSize, uint32_t usageBits) = 0;

  /// Creates a texture. @param id Identifier to register it under. @param width Width in texels.
  /// @param height Height in texels. @param formatCode Encoded \ref TextureFormat.
  /// @param usageBits Encoded \ref TextureUsage mask.
  virtual BridgeStatus createTexture(BrowserObjectId id, uint32_t width, uint32_t height,
                                     uint32_t formatCode, uint32_t usageBits) = 0;

  /// Creates a view covering the whole texture. @param id Identifier to register it under.
  /// @param textureId Texture to view.
  virtual BridgeStatus createTextureView(BrowserObjectId id, BrowserObjectId textureId) = 0;

  /// Creates a sampler. @param id Identifier to register it under.
  /// @param magFilterCode Encoded magnification \ref FilterMode.
  /// @param minFilterCode Encoded minification \ref FilterMode.
  /// @param addressUCode Encoded U \ref AddressMode. @param addressVCode Encoded V
  /// \ref AddressMode.
  virtual BridgeStatus createSampler(BrowserObjectId id, uint32_t magFilterCode,
                                     uint32_t minFilterCode, uint32_t addressUCode,
                                     uint32_t addressVCode) = 0;

  /// Creates a bind group layout. @param id Identifier to register it under.
  /// @param entries Layout entries.
  virtual BridgeStatus createBindGroupLayout(
      BrowserObjectId id, std::span<const BrowserBindGroupLayoutEntry> entries) = 0;

  /// Creates a bind group. @param id Identifier to register it under.
  /// @param layoutId Layout the entries match. @param entries Bound resources.
  virtual BridgeStatus createBindGroup(BrowserObjectId id, BrowserObjectId layoutId,
                                       std::span<const BrowserBindGroupEntry> entries) = 0;

  /// Creates a pipeline layout. @param id Identifier to register it under.
  /// @param groupLayoutIds Bind group layouts, by group index.
  virtual BridgeStatus createPipelineLayout(BrowserObjectId id,
                                            std::span<const BrowserObjectId> groupLayoutIds) = 0;

  /// Creates a shader module from trusted build output. @param id Identifier to register it
  /// under. @param wgsl Shader source text.
  virtual BridgeStatus createShaderModule(BrowserObjectId id, std::string_view wgsl) = 0;

  /// Creates a render pipeline. @param id Identifier to register it under.
  /// @param request Pipeline description.
  virtual BridgeStatus createRenderPipeline(BrowserObjectId id,
                                            const BrowserRenderPipelineRequest& request) = 0;

  /// Creates a compute pipeline. @param id Identifier to register it under.
  /// @param request Pipeline description.
  virtual BridgeStatus createComputePipeline(BrowserObjectId id,
                                             const BrowserComputePipelineRequest& request) = 0;

  /// Releases a browser object and unregisters its identifier, which is never reissued.
  /// @param kind Kind the identifier must name. @param id Identifier to release.
  virtual BridgeStatus destroyObject(BrowserObjectKind kind, BrowserObjectId id) = 0;

  // Queue writes.

  /// Writes bytes into a buffer. @param bufferId Destination buffer.
  /// @param offsetBytes Destination byte offset. @param data Payload.
  virtual BridgeStatus writeBuffer(BrowserObjectId bufferId, uint64_t offsetBytes,
                                   std::span<const uint8_t> data) = 0;

  /// Writes texel rows into a texture. @param textureId Destination texture. @param data Payload.
  /// @param layout Row layout of \p data. @param region Destination rectangle; its source
  /// coordinates are unused.
  virtual BridgeStatus writeTexture(BrowserObjectId textureId, std::span<const uint8_t> data,
                                    const BrowserTexelLayout& layout,
                                    const BrowserCopyRegion& region) = 0;

  // Command recording. One submission opens with beginCommandBuffer, replays its commands in
  // recording order, and closes with endCommandBuffer, which finishes the encoder and submits it.

  /// Opens recording for the submission with \p submissionSerial. A recording left open by an
  /// earlier submission that failed partway is discarded rather than continued, so a refused
  /// command can never end up submitted as part of the next frame.
  /// @param submissionSerial Serial the runtime assigned.
  virtual BridgeStatus beginCommandBuffer(uint64_t submissionSerial) = 0;

  /// Begins a render pass. @param colorAttachments Attachments of the pass.
  virtual BridgeStatus beginRenderPass(
      std::span<const BrowserColorAttachment> colorAttachments) = 0;

  /// Ends the open render pass.
  virtual BridgeStatus endRenderPass() = 0;

  /// Begins a compute pass.
  virtual BridgeStatus beginComputePass() = 0;

  /// Ends the open compute pass.
  virtual BridgeStatus endComputePass() = 0;

  /// Sets the render pipeline of the open render pass. @param pipelineId Render pipeline.
  virtual BridgeStatus setRenderPipeline(BrowserObjectId pipelineId) = 0;

  /// Sets the compute pipeline of the open compute pass. @param pipelineId Compute pipeline.
  virtual BridgeStatus setComputePipeline(BrowserObjectId pipelineId) = 0;

  /// Binds a group in the open pass. @param index Group index. @param bindGroupId Bind group.
  virtual BridgeStatus setBindGroup(uint32_t index, BrowserObjectId bindGroupId) = 0;

  /// Binds a vertex buffer. @param slot Vertex buffer slot. @param bufferId Buffer.
  /// @param offsetBytes Byte offset of the first element.
  virtual BridgeStatus setVertexBuffer(uint32_t slot, BrowserObjectId bufferId,
                                       uint64_t offsetBytes) = 0;

  /// Binds an index buffer. @param bufferId Buffer. @param indexFormatCode Encoded
  /// \ref IndexFormat. @param offsetBytes Byte offset of the first index.
  virtual BridgeStatus setIndexBuffer(BrowserObjectId bufferId, uint32_t indexFormatCode,
                                      uint64_t offsetBytes) = 0;

  /// Sets the scissor rectangle. @param x Left edge. @param y Top edge. @param width Width.
  /// @param height Height.
  virtual BridgeStatus setScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

  /// Sets the viewport. @param x Left edge. @param y Top edge. @param width Width.
  /// @param height Height. @param minDepth Minimum depth. @param maxDepth Maximum depth.
  virtual BridgeStatus setViewport(float x, float y, float width, float height, float minDepth,
                                   float maxDepth) = 0;

  /// Draws non-indexed geometry. @param vertexCount Vertices. @param instanceCount Instances.
  /// @param firstVertex First vertex. @param firstInstance First instance.
  virtual BridgeStatus draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                            uint32_t firstInstance) = 0;

  /// Draws indexed geometry. @param indexCount Indices. @param instanceCount Instances.
  /// @param firstIndex First index. @param baseVertex Value added to every index.
  /// @param firstInstance First instance.
  virtual BridgeStatus drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                   int32_t baseVertex, uint32_t firstInstance) = 0;

  /// Dispatches compute workgroups. @param countX Workgroups along X. @param countY Workgroups
  /// along Y. @param countZ Workgroups along Z.
  virtual BridgeStatus dispatchWorkgroups(uint32_t countX, uint32_t countY, uint32_t countZ) = 0;

  /// Copies texels into a buffer. @param textureId Source texture. @param bufferId Destination
  /// buffer. @param layout Destination row layout. @param region Copied rectangle.
  virtual BridgeStatus copyTextureToBuffer(BrowserObjectId textureId, BrowserObjectId bufferId,
                                           const BrowserTexelLayout& layout,
                                           const BrowserCopyRegion& region) = 0;

  /// Copies texels between textures. @param sourceTextureId Source. @param destinationTextureId
  /// Destination. @param region Copied rectangle with both origins.
  virtual BridgeStatus copyTextureToTexture(BrowserObjectId sourceTextureId,
                                            BrowserObjectId destinationTextureId,
                                            const BrowserCopyRegion& region) = 0;

  /// Finishes the open command buffer and submits it.
  /// @param submissionSerial Serial the runtime assigned.
  virtual BridgeStatus endCommandBuffer(uint64_t submissionSerial) = 0;

  // Host mapping.

  /// Begins mapping a buffer range for host reads. @param mappingId Identifier to register the
  /// mapping under. @param bufferId Buffer to map. @param offsetBytes Byte offset of the range.
  /// @param byteCount Length of the range.
  virtual BridgeStatus mapBufferAsync(BrowserObjectId mappingId, BrowserObjectId bufferId,
                                      uint64_t offsetBytes, uint64_t byteCount) = 0;

  /// State of a mapping the browser has been asked for. @param mappingId Mapping to query.
  virtual MapSliceState mappingState(BrowserObjectId mappingId) const = 0;

  /// Bytes of a completed mapping, valid until the mapping is released. Returns an empty span
  /// with a non-success status when the mapping is not readable. @param mappingId Mapping to
  /// read. @param bytes Receives the mapped bytes on success.
  virtual BridgeStatus mappedBytes(BrowserObjectId mappingId,
                                   std::span<const uint8_t>& bytes) const = 0;

  /// Releases a mapping and unregisters its identifier. @param mappingId Mapping to release.
  virtual BridgeStatus unmapBuffer(BrowserObjectId mappingId) = 0;

  // Presentation.

  /// Creates a surface over the canvas named by \p canvasSelector.
  /// @param id Identifier to register it under. @param canvasSelector CSS selector naming the
  /// canvas.
  virtual BridgeStatus createSurface(BrowserObjectId id, std::string_view canvasSelector) = 0;

  /// Reports what a surface supports. @param surfaceId Surface to query.
  /// @param capabilities Receives the capabilities on success.
  virtual BridgeStatus surfaceCapabilities(BrowserObjectId surfaceId,
                                           BrowserSurfaceCapabilities& capabilities) const = 0;

  /// Configures how a surface presents. @param surfaceId Surface to configure.
  /// @param formatCode Encoded \ref TextureFormat. @param usageBits Encoded \ref TextureUsage
  /// mask. @param width Width in texels. @param height Height in texels.
  /// @param alphaModeCode Encoded \ref SurfaceAlphaMode.
  virtual BridgeStatus configureSurface(BrowserObjectId surfaceId, uint32_t formatCode,
                                        uint32_t usageBits, uint32_t width, uint32_t height,
                                        uint32_t alphaModeCode) = 0;

  /// Takes the surface's texture for this frame and registers it under \p textureId.
  /// @param surfaceId Surface to acquire from. @param textureId Identifier for the frame texture.
  /// @param status Receives what the surface reported.
  virtual BridgeStatus acquireCurrentTexture(BrowserObjectId surfaceId, BrowserObjectId textureId,
                                             SurfaceStatus& status) = 0;

  /// Gives back the acquired texture without showing it. @param surfaceId Surface holding it.
  virtual BridgeStatus abandonCurrentTexture(BrowserObjectId surfaceId) = 0;

protected:
  /// Constructor for implementations.
  BrowserBridge();
};

}  // namespace donner::gpu::browser
