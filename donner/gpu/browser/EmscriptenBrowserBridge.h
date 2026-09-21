#pragma once
/// @file
/// \c donner::gpu::browser::EmscriptenBrowserBridge - the bridge implementation that calls the
/// browser.

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

/**
 * The \ref BrowserBridge implementation that actually calls the browser's GPU service.
 *
 * Every method forwards to one entry point of `library_donner_gpu.js`, which owns the browser
 * objects and performs the identifier, kind and ownership checks on its side. This class holds no
 * browser state of its own beyond the mapped bytes it copies out of the browser's heap, so what
 * exists is recorded in one place rather than in two that could disagree.
 *
 * \ref beginDeviceRequest compares \ref ProtocolCodeTable against the table the library holds
 * before asking for a device, so the two halves agree on what their numbers mean before anything
 * is built on them. See BrowserWireCodes.h for what that check covers and what it does not.
 *
 * Sizes and offsets cross as doubles rather than 64-bit integers. Every value that crosses is
 * bounded far below the 2^53 a double represents exactly - the runtime caps buffers at 1 GiB and
 * textures at 16384 texels a side - and a double is the one numeric type both sides agree on
 * without a 64-bit calling-convention dependency.
 *
 * WebAssembly only: it exists to call a browser.
 */
class EmscriptenBrowserBridge final : public BrowserBridge {
public:
  /// Constructs a bridge over the browser's GPU service. No browser call happens until
  /// \ref beginDeviceRequest.
  EmscriptenBrowserBridge();

  /// Destructor; releases the browser device and every object still registered under it.
  ~EmscriptenBrowserBridge() override;

  BridgeStatus beginDeviceRequest() override;
  BrowserDeviceRequestState deviceRequestState() const override;
  RcString deviceRequestError() const override;

  bool ownsDevice() const override;
  bool isDeviceLost() const override;
  RcString deviceLostReason() const override;
  uint64_t completedSerial() const override;

  BridgeStatus createBuffer(BrowserObjectId id, uint64_t byteSize, uint32_t usageBits) override;
  BridgeStatus createTexture(BrowserObjectId id, uint32_t width, uint32_t height,
                             uint32_t formatCode, uint32_t usageBits) override;
  BridgeStatus createTextureView(BrowserObjectId id, BrowserObjectId textureId) override;
  BridgeStatus createSampler(BrowserObjectId id, uint32_t magFilterCode, uint32_t minFilterCode,
                             uint32_t addressUCode, uint32_t addressVCode) override;
  BridgeStatus createBindGroupLayout(BrowserObjectId id,
                                     std::span<const BrowserBindGroupLayoutEntry> entries) override;
  BridgeStatus createBindGroup(BrowserObjectId id, BrowserObjectId layoutId,
                               std::span<const BrowserBindGroupEntry> entries) override;
  BridgeStatus createPipelineLayout(BrowserObjectId id,
                                    std::span<const BrowserObjectId> groupLayoutIds) override;
  BridgeStatus createShaderModule(BrowserObjectId id, std::string_view wgsl) override;
  BridgeStatus createRenderPipeline(BrowserObjectId id,
                                    const BrowserRenderPipelineRequest& request) override;
  BridgeStatus createComputePipeline(BrowserObjectId id,
                                     const BrowserComputePipelineRequest& request) override;
  BridgeStatus destroyObject(BrowserObjectKind kind, BrowserObjectId id) override;

  BridgeStatus writeBuffer(BrowserObjectId bufferId, uint64_t offsetBytes,
                           std::span<const uint8_t> data) override;
  BridgeStatus writeTexture(BrowserObjectId textureId, std::span<const uint8_t> data,
                            const BrowserTexelLayout& layout,
                            const BrowserCopyRegion& region) override;

  BridgeStatus beginCommandBuffer(uint64_t submissionSerial, uint32_t commandBufferIndex) override;
  BridgeStatus beginRenderPass(std::span<const BrowserColorAttachment> colorAttachments) override;
  BridgeStatus endRenderPass() override;
  BridgeStatus beginComputePass() override;
  BridgeStatus endComputePass() override;
  BridgeStatus setRenderPipeline(BrowserObjectId pipelineId) override;
  BridgeStatus setComputePipeline(BrowserObjectId pipelineId) override;
  BridgeStatus setBindGroup(uint32_t index, BrowserObjectId bindGroupId) override;
  BridgeStatus setVertexBuffer(uint32_t slot, BrowserObjectId bufferId,
                               uint64_t offsetBytes) override;
  BridgeStatus setIndexBuffer(BrowserObjectId bufferId, uint32_t indexFormatCode,
                              uint64_t offsetBytes) override;
  BridgeStatus setScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
  BridgeStatus setViewport(float x, float y, float width, float height, float minDepth,
                           float maxDepth) override;
  BridgeStatus draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                    uint32_t firstInstance) override;
  BridgeStatus drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                           int32_t baseVertex, uint32_t firstInstance) override;
  BridgeStatus dispatchWorkgroups(uint32_t countX, uint32_t countY, uint32_t countZ) override;
  BridgeStatus copyTextureToBuffer(BrowserObjectId textureId, BrowserObjectId bufferId,
                                   const BrowserTexelLayout& layout,
                                   const BrowserCopyRegion& region) override;
  BridgeStatus copyTextureToTexture(BrowserObjectId sourceTextureId,
                                    BrowserObjectId destinationTextureId,
                                    const BrowserCopyRegion& region) override;
  BridgeStatus endCommandBuffer(uint64_t submissionSerial) override;
  BridgeStatus submitCommandBuffers(uint64_t submissionSerial) override;

  BridgeStatus mapBufferAsync(BrowserObjectId mappingId, BrowserObjectId bufferId,
                              uint64_t offsetBytes, uint64_t byteCount) override;
  MapSliceState mappingState(BrowserObjectId mappingId) const override;
  void yieldToBrowser(double seconds) override;
  BridgeStatus mappedBytes(BrowserObjectId mappingId,
                           std::span<const uint8_t>& bytes) const override;
  BridgeStatus unmapBuffer(BrowserObjectId mappingId) override;

  BridgeStatus createSurface(BrowserObjectId id, std::string_view canvasSelector) override;
  BridgeStatus surfaceCapabilities(BrowserObjectId surfaceId,
                                   BrowserSurfaceCapabilities& capabilities) const override;
  BridgeStatus configureSurface(BrowserObjectId surfaceId, uint32_t formatCode, uint32_t usageBits,
                                uint32_t width, uint32_t height, uint32_t alphaModeCode) override;
  BridgeStatus acquireCurrentTexture(BrowserObjectId surfaceId, BrowserObjectId textureId,
                                     SurfaceStatus& status) override;
  BridgeStatus abandonCurrentTexture(BrowserObjectId surfaceId) override;

private:
  /// A mapping this bridge has copied out of the browser's heap.
  struct MappedRange {
    uint64_t byteCount = 0;      //!< Length the mapping was requested with.
    std::vector<uint8_t> bytes;  //!< Copy of the mapped range; empty until it is read.
    bool copied = false;         //!< Whether \ref bytes holds the copy.
  };

  /// Ranges this bridge has been asked to map, by identifier.
  ///
  /// Mutable because reading a completed mapping copies it out of the browser's heap once, and
  /// reading is a const operation on this interface: the copy is a cache of a decision the
  /// browser already made, not state of this bridge's own.
  mutable std::map<BrowserObjectId, MappedRange> mappings_;
};

}  // namespace donner::gpu::browser
