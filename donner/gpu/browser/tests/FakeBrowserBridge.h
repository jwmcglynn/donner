#pragma once
/// @file
/// \c donner::gpu::browser::FakeBrowserBridge - a browser side without a browser, for tests.

#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

/**
 * A \ref BrowserBridge that behaves like the browser side without being one.
 *
 * It keeps its own registry of the objects it has been asked to create and applies the same
 * identifier, kind and ownership checks the real browser side does, so a test driving
 * \ref BrowserDevice through it exercises both ends of the contract rather than only the near
 * one. Every accepted call is appended to \ref calls as one deterministic line, which is what
 * lets a test assert that a recorded command stream reached the browser in recording order and
 * with the arguments it was recorded with.
 *
 * Outcomes are programmable: \ref failOperation names an operation that should be refused and
 * \ref failStatus says how, so a test covers a browser-side refusal without needing a browser
 * that refuses.
 */
class FakeBrowserBridge final : public BrowserBridge {
public:
  /// Constructs a bridge whose device request is already settled and ready.
  FakeBrowserBridge() = default;

  /// Destructor.
  ~FakeBrowserBridge() override = default;

  // Programmable state. A test sets these directly; they are the browser's behavior, not the
  // bridge's own.

  /// What \ref deviceRequestState reports.
  BrowserDeviceRequestState requestState = BrowserDeviceRequestState::Ready;
  /// What \ref deviceRequestError reports.
  RcString requestError;
  /// What \ref beginDeviceRequest returns.
  BridgeStatus beginStatus = BridgeStatus::Success;
  /// Whether the calling context owns the device.
  bool owned = true;
  /// Whether the browser has reported the device lost.
  bool lost = false;
  /// What the browser said when it reported the loss.
  RcString lostReason;
  /// What \ref completedSerial reports.
  uint64_t completed = 0;
  /// Operation name to refuse, matched against the leading word of a recorded line. Empty
  /// refuses nothing.
  std::string failOperation;
  /// How \ref failOperation is refused.
  BridgeStatus failStatus = BridgeStatus::Failed;
  /// What \ref surfaceCapabilities reports.
  BrowserSurfaceCapabilities capabilities;
  /// What \ref acquireCurrentTexture reports.
  SurfaceStatus acquireStatus = SurfaceStatus::Success;

  /// Every accepted call, one deterministic line each, in order.
  ///
  /// Shared for the same reason the registry below is: the device owns the bridge, so a test that
  /// looks at what teardown did would otherwise be reading a destroyed vector.
  std::shared_ptr<std::vector<std::string>> calls = std::make_shared<std::vector<std::string>>();

  /// The objects this bridge holds, by identifier.
  ///
  /// Shared rather than owned outright because the device owns the bridge: a test that watches
  /// what device teardown released needs the registry to outlive the bridge that kept it.
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects =
      std::make_shared<std::map<BrowserObjectId, BrowserObjectKind>>();

  /// Marks the mapping \p mappingId as holding \p bytes and ready to read.
  /// @param mappingId Mapping to complete. @param bytes Bytes the host will see.
  void completeMapping(BrowserObjectId mappingId, std::vector<uint8_t> bytes) {
    mappings_[mappingId] = Mapping{MapSliceState::Ready, std::move(bytes)};
  }

  /// Sets the state of the mapping \p mappingId without giving it bytes.
  /// @param mappingId Mapping to update. @param state State to report.
  void setMappingState(BrowserObjectId mappingId, MapSliceState state) {
    mappings_[mappingId].state = state;
  }

  /// Whether \p id names a live object of \p kind in this bridge's registry.
  /// @param kind Expected kind. @param id Identifier to check.
  bool hasObject(BrowserObjectKind kind, BrowserObjectId id) const {
    const auto it = objects->find(id);
    return it != objects->end() && it->second == kind;
  }

  /// Number of objects this bridge currently holds.
  size_t objectCount() const { return objects->size(); }

  BridgeStatus beginDeviceRequest() override {
    if (beginStatus != BridgeStatus::Success) {
      return beginStatus;
    }
    calls->push_back("beginDeviceRequest");
    return BridgeStatus::Success;
  }

  BrowserDeviceRequestState deviceRequestState() const override { return requestState; }

  RcString deviceRequestError() const override { return requestError; }

  bool ownsDevice() const override { return owned; }

  bool isDeviceLost() const override { return lost; }

  RcString deviceLostReason() const override { return lostReason; }

  uint64_t completedSerial() const override { return completed; }

  BridgeStatus createBuffer(BrowserObjectId id, uint64_t byteSize, uint32_t usageBits) override {
    return create(BrowserObjectKind::Buffer, id,
                  std::format("createBuffer id={} byteSize={} usage={}", id, byteSize, usageBits));
  }

  BridgeStatus createTexture(BrowserObjectId id, uint32_t width, uint32_t height,
                             uint32_t formatCode, uint32_t usageBits) override {
    return create(BrowserObjectKind::Texture, id,
                  std::format("createTexture id={} size={}x{} format={} usage={}", id, width,
                              height, formatCode, usageBits));
  }

  BridgeStatus createTextureView(BrowserObjectId id, BrowserObjectId textureId) override {
    if (const BridgeStatus status = require(BrowserObjectKind::Texture, textureId);
        status != BridgeStatus::Success) {
      return status;
    }
    return create(BrowserObjectKind::TextureView, id,
                  std::format("createTextureView id={} texture={}", id, textureId));
  }

  BridgeStatus createSampler(BrowserObjectId id, uint32_t magFilterCode, uint32_t minFilterCode,
                             uint32_t addressUCode, uint32_t addressVCode) override {
    return create(BrowserObjectKind::Sampler, id,
                  std::format("createSampler id={} mag={} min={} addressU={} addressV={}", id,
                              magFilterCode, minFilterCode, addressUCode, addressVCode));
  }

  BridgeStatus createBindGroupLayout(
      BrowserObjectId id, std::span<const BrowserBindGroupLayoutEntry> entries) override {
    std::string line = std::format("createBindGroupLayout id={} entries=[", id);
    for (const BrowserBindGroupLayoutEntry& entry : entries) {
      line += std::format("(binding={} visibility={} type={} storageFormat={})", entry.binding,
                          entry.visibilityBits, entry.bindingTypeCode, entry.storageTextureFormat);
    }
    line += "]";
    return create(BrowserObjectKind::BindGroupLayout, id, line);
  }

  BridgeStatus createBindGroup(BrowserObjectId id, BrowserObjectId layoutId,
                               std::span<const BrowserBindGroupEntry> entries) override {
    if (const BridgeStatus status = require(BrowserObjectKind::BindGroupLayout, layoutId);
        status != BridgeStatus::Success) {
      return status;
    }
    std::string line = std::format("createBindGroup id={} layout={} entries=[", id, layoutId);
    for (const BrowserBindGroupEntry& entry : entries) {
      const BrowserObjectKind kind = KindOfBinding(entry.resource);
      if (const BridgeStatus status = require(kind, entry.resourceId);
          status != BridgeStatus::Success) {
        return status;
      }
      line += std::format("(binding={} resource={} offset={} size={})", entry.binding,
                          entry.resourceId, entry.offsetBytes, entry.sizeBytes);
    }
    line += "]";
    return create(BrowserObjectKind::BindGroup, id, line);
  }

  BridgeStatus createPipelineLayout(BrowserObjectId id,
                                    std::span<const BrowserObjectId> groupLayoutIds) override {
    std::string line = std::format("createPipelineLayout id={} groups=[", id);
    for (const BrowserObjectId layoutId : groupLayoutIds) {
      if (const BridgeStatus status = require(BrowserObjectKind::BindGroupLayout, layoutId);
          status != BridgeStatus::Success) {
        return status;
      }
      line += std::format("{},", layoutId);
    }
    line += "]";
    return create(BrowserObjectKind::PipelineLayout, id, line);
  }

  BridgeStatus createShaderModule(BrowserObjectId id, std::string_view wgsl) override {
    return create(BrowserObjectKind::ShaderModule, id,
                  std::format("createShaderModule id={} wgslBytes={}", id, wgsl.size()));
  }

  BridgeStatus createRenderPipeline(BrowserObjectId id,
                                    const BrowserRenderPipelineRequest& request) override {
    for (const std::pair<BrowserObjectKind, BrowserObjectId> dependency :
         {std::pair{BrowserObjectKind::PipelineLayout, request.layoutId},
          std::pair{BrowserObjectKind::ShaderModule, request.vertexModuleId},
          std::pair{BrowserObjectKind::ShaderModule, request.fragmentModuleId}}) {
      if (const BridgeStatus status = require(dependency.first, dependency.second);
          status != BridgeStatus::Success) {
        return status;
      }
    }
    return create(
        BrowserObjectKind::RenderPipeline, id,
        std::format("createRenderPipeline id={} layout={} vertex={}:{} fragment={}:{} "
                    "vertexBuffers={} targets={} topology={} cull={}",
                    id, request.layoutId, request.vertexModuleId, request.vertexEntryPoint.str(),
                    request.fragmentModuleId, request.fragmentEntryPoint.str(),
                    request.vertexBuffers.size(), request.colorTargets.size(), request.topologyCode,
                    request.cullModeCode));
  }

  BridgeStatus createComputePipeline(BrowserObjectId id,
                                     const BrowserComputePipelineRequest& request) override {
    for (const std::pair<BrowserObjectKind, BrowserObjectId> dependency :
         {std::pair{BrowserObjectKind::PipelineLayout, request.layoutId},
          std::pair{BrowserObjectKind::ShaderModule, request.moduleId}}) {
      if (const BridgeStatus status = require(dependency.first, dependency.second);
          status != BridgeStatus::Success) {
        return status;
      }
    }
    return create(BrowserObjectKind::ComputePipeline, id,
                  std::format("createComputePipeline id={} layout={} module={}:{}", id,
                              request.layoutId, request.moduleId, request.entryPoint.str()));
  }

  BridgeStatus destroyObject(BrowserObjectKind kind, BrowserObjectId id) override {
    // Releases are not refused on a lost device: a lost device still has to free what it holds, and
    // the browser side takes them for the same reason.
    if (!owned) {
      return BridgeStatus::NotOwner;
    }
    if (!failOperation.empty() && failOperation == "destroyObject") {
      return failStatus;
    }
    if (const BridgeStatus status = require(kind, id); status != BridgeStatus::Success) {
      return status;
    }
    objects->erase(id);
    mappings_.erase(id);
    calls->push_back(std::format("destroyObject kind={} id={}", BrowserObjectKindName(kind), id));
    return BridgeStatus::Success;
  }

  BridgeStatus writeBuffer(BrowserObjectId bufferId, uint64_t offsetBytes,
                           std::span<const uint8_t> data) override {
    return operate(
        std::format("writeBuffer buffer={} offset={} bytes={}", bufferId, offsetBytes, data.size()),
        BrowserObjectKind::Buffer, bufferId);
  }

  BridgeStatus writeTexture(BrowserObjectId textureId, std::span<const uint8_t> data,
                            const BrowserTexelLayout& layout,
                            const BrowserCopyRegion& region) override {
    return operate(
        std::format("writeTexture texture={} bytes={} offset={} bytesPerRow={} rowsPerImage={} "
                    "destination=({},{}) size={}x{}",
                    textureId, data.size(), layout.offsetBytes, layout.bytesPerRow,
                    layout.rowsPerImage, region.destinationX, region.destinationY, region.width,
                    region.height),
        BrowserObjectKind::Texture, textureId);
  }

  BridgeStatus beginCommandBuffer(uint64_t submissionSerial) override {
    const BridgeStatus status =
        operate(std::format("beginCommandBuffer serial={}", submissionSerial));
    if (status == BridgeStatus::Success) {
      // A recording left open by a submission that was refused partway is discarded here rather
      // than continued, so nothing recorded before the refusal can reach the queue.
      encoderOpen_ = true;
      passOpen_ = false;
    }
    return status;
  }

  BridgeStatus beginRenderPass(std::span<const BrowserColorAttachment> colorAttachments) override {
    if (!encoderOpen_ || passOpen_) {
      return BridgeStatus::Failed;
    }
    std::string line = "beginRenderPass attachments=[";
    for (const BrowserColorAttachment& attachment : colorAttachments) {
      if (const BridgeStatus status = require(BrowserObjectKind::TextureView, attachment.viewId);
          status != BridgeStatus::Success) {
        return status;
      }
      line += std::format("(view={} load={} store={} clear=[{:.3f},{:.3f},{:.3f},{:.3f}])",
                          attachment.viewId, attachment.loadOpCode, attachment.storeOpCode,
                          attachment.clearColor[0], attachment.clearColor[1],
                          attachment.clearColor[2], attachment.clearColor[3]);
    }
    line += "]";
    const BridgeStatus status = operate(line);
    if (status == BridgeStatus::Success) {
      passOpen_ = true;
    }
    return status;
  }

  BridgeStatus endRenderPass() override {
    const BridgeStatus status = requirePass("endRenderPass");
    if (status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back("endRenderPass");
    passOpen_ = false;
    return BridgeStatus::Success;
  }

  BridgeStatus beginComputePass() override {
    if (!encoderOpen_ || passOpen_) {
      return BridgeStatus::Failed;
    }
    const BridgeStatus status = operate("beginComputePass");
    if (status == BridgeStatus::Success) {
      passOpen_ = true;
    }
    return status;
  }

  BridgeStatus endComputePass() override {
    const BridgeStatus status = requirePass("endComputePass");
    if (status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back("endComputePass");
    passOpen_ = false;
    return BridgeStatus::Success;
  }

  BridgeStatus setRenderPipeline(BrowserObjectId pipelineId) override {
    return operateInPass(std::format("setRenderPipeline pipeline={}", pipelineId),
                         BrowserObjectKind::RenderPipeline, pipelineId);
  }

  BridgeStatus setComputePipeline(BrowserObjectId pipelineId) override {
    return operateInPass(std::format("setComputePipeline pipeline={}", pipelineId),
                         BrowserObjectKind::ComputePipeline, pipelineId);
  }

  BridgeStatus setBindGroup(uint32_t index, BrowserObjectId bindGroupId) override {
    return operateInPass(std::format("setBindGroup index={} bindGroup={}", index, bindGroupId),
                         BrowserObjectKind::BindGroup, bindGroupId);
  }

  BridgeStatus setVertexBuffer(uint32_t slot, BrowserObjectId bufferId,
                               uint64_t offsetBytes) override {
    return operateInPass(
        std::format("setVertexBuffer slot={} buffer={} offset={}", slot, bufferId, offsetBytes),
        BrowserObjectKind::Buffer, bufferId);
  }

  BridgeStatus setIndexBuffer(BrowserObjectId bufferId, uint32_t indexFormatCode,
                              uint64_t offsetBytes) override {
    return operateInPass(std::format("setIndexBuffer buffer={} format={} offset={}", bufferId,
                                     indexFormatCode, offsetBytes),
                         BrowserObjectKind::Buffer, bufferId);
  }

  BridgeStatus setScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override {
    return operateInPass(std::format("setScissorRect x={} y={} size={}x{}", x, y, width, height));
  }

  BridgeStatus setViewport(float x, float y, float width, float height, float minDepth,
                           float maxDepth) override {
    return operateInPass(
        std::format("setViewport x={:.3f} y={:.3f} size={:.3f}x{:.3f} depth={:.3f}..{:.3f}", x, y,
                    width, height, minDepth, maxDepth));
  }

  BridgeStatus draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                    uint32_t firstInstance) override {
    return operateInPass(
        std::format("draw vertexCount={} instanceCount={} firstVertex={} "
                    "firstInstance={}",
                    vertexCount, instanceCount, firstVertex, firstInstance));
  }

  BridgeStatus drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                           int32_t baseVertex, uint32_t firstInstance) override {
    return operateInPass(
        std::format("drawIndexed indexCount={} instanceCount={} firstIndex={} "
                    "baseVertex={} firstInstance={}",
                    indexCount, instanceCount, firstIndex, baseVertex, firstInstance));
  }

  BridgeStatus dispatchWorkgroups(uint32_t countX, uint32_t countY, uint32_t countZ) override {
    return operateInPass(std::format("dispatchWorkgroups count={}x{}x{}", countX, countY, countZ));
  }

  BridgeStatus copyTextureToBuffer(BrowserObjectId textureId, BrowserObjectId bufferId,
                                   const BrowserTexelLayout& layout,
                                   const BrowserCopyRegion& region) override {
    if (const BridgeStatus status = require(BrowserObjectKind::Texture, textureId);
        status != BridgeStatus::Success) {
      return status;
    }
    if (passOpen_) {
      return BridgeStatus::Failed;
    }
    return operateInEncoder(std::format("copyTextureToBuffer texture={} buffer={} offset={} "
                                        "bytesPerRow={} rowsPerImage={} size={}x{}",
                                        textureId, bufferId, layout.offsetBytes, layout.bytesPerRow,
                                        layout.rowsPerImage, region.width, region.height),
                            BrowserObjectKind::Buffer, bufferId);
  }

  BridgeStatus copyTextureToTexture(BrowserObjectId sourceTextureId,
                                    BrowserObjectId destinationTextureId,
                                    const BrowserCopyRegion& region) override {
    if (const BridgeStatus status = require(BrowserObjectKind::Texture, sourceTextureId);
        status != BridgeStatus::Success) {
      return status;
    }
    if (passOpen_) {
      return BridgeStatus::Failed;
    }
    return operateInEncoder(
        std::format("copyTextureToTexture source={} destination={} sourceOrigin=({},{}) "
                    "destinationOrigin=({},{}) size={}x{}",
                    sourceTextureId, destinationTextureId, region.sourceX, region.sourceY,
                    region.destinationX, region.destinationY, region.width, region.height),
        BrowserObjectKind::Texture, destinationTextureId);
  }

  BridgeStatus endCommandBuffer(uint64_t submissionSerial) override {
    const std::string line = std::format("endCommandBuffer serial={}", submissionSerial);
    if (const BridgeStatus status = requireEncoder(line); status != BridgeStatus::Success) {
      return status;
    }
    if (passOpen_) {
      return BridgeStatus::Failed;
    }
    calls->push_back(line);
    encoderOpen_ = false;
    return BridgeStatus::Success;
  }

  BridgeStatus mapBufferAsync(BrowserObjectId mappingId, BrowserObjectId bufferId,
                              uint64_t offsetBytes, uint64_t byteCount) override {
    if (const BridgeStatus status = require(BrowserObjectKind::Buffer, bufferId);
        status != BridgeStatus::Success) {
      return status;
    }
    const BridgeStatus status =
        create(BrowserObjectKind::BufferMapping, mappingId,
               std::format("mapBufferAsync mapping={} buffer={} offset={} bytes={}", mappingId,
                           bufferId, offsetBytes, byteCount));
    if (status == BridgeStatus::Success) {
      mappings_[mappingId] = Mapping{MapSliceState::Pending, {}};
    }
    return status;
  }

  MapSliceState mappingState(BrowserObjectId mappingId) const override {
    const auto it = mappings_.find(mappingId);
    if (it == mappings_.end()) {
      return MapSliceState::Failed;
    }
    return it->second.state;
  }

  BridgeStatus mappedBytes(BrowserObjectId mappingId,
                           std::span<const uint8_t>& bytes) const override {
    const auto registered = objects->find(mappingId);
    if (registered == objects->end()) {
      return BridgeStatus::UnknownObject;
    }
    if (registered->second != BrowserObjectKind::BufferMapping) {
      return BridgeStatus::WrongObjectKind;
    }
    const auto it = mappings_.find(mappingId);
    if (it == mappings_.end() || it->second.state != MapSliceState::Ready) {
      return BridgeStatus::Failed;
    }
    bytes = std::span<const uint8_t>(it->second.bytes);
    return BridgeStatus::Success;
  }

  BridgeStatus unmapBuffer(BrowserObjectId mappingId) override {
    if (const BridgeStatus status = require(BrowserObjectKind::BufferMapping, mappingId);
        status != BridgeStatus::Success) {
      return status;
    }
    objects->erase(mappingId);
    mappings_.erase(mappingId);
    calls->push_back(std::format("unmapBuffer mapping={}", mappingId));
    return BridgeStatus::Success;
  }

  BridgeStatus createSurface(BrowserObjectId id, std::string_view canvasSelector) override {
    return create(BrowserObjectKind::Surface, id,
                  std::format("createSurface id={} canvas={}", id, canvasSelector));
  }

  BridgeStatus surfaceCapabilities(BrowserObjectId surfaceId,
                                   BrowserSurfaceCapabilities& reported) const override {
    const auto it = objects->find(surfaceId);
    if (it == objects->end()) {
      return BridgeStatus::UnknownObject;
    }
    if (it->second != BrowserObjectKind::Surface) {
      return BridgeStatus::WrongObjectKind;
    }
    reported = capabilities;
    return BridgeStatus::Success;
  }

  BridgeStatus configureSurface(BrowserObjectId surfaceId, uint32_t formatCode, uint32_t usageBits,
                                uint32_t width, uint32_t height, uint32_t alphaModeCode) override {
    return operate(std::format("configureSurface surface={} format={} usage={} size={}x{} "
                               "alphaMode={}",
                               surfaceId, formatCode, usageBits, width, height, alphaModeCode),
                   BrowserObjectKind::Surface, surfaceId);
  }

  BridgeStatus acquireCurrentTexture(BrowserObjectId surfaceId, BrowserObjectId textureId,
                                     SurfaceStatus& status) override {
    if (const BridgeStatus bridgeStatus = require(BrowserObjectKind::Surface, surfaceId);
        bridgeStatus != BridgeStatus::Success) {
      return bridgeStatus;
    }
    status = acquireStatus;
    if (acquireStatus != SurfaceStatus::Success && acquireStatus != SurfaceStatus::Outdated) {
      // No frame came back, so nothing is registered under the identifier the runtime minted for
      // one; recording the call still shows the acquisition was attempted.
      calls->push_back(std::format("acquireCurrentTexture surface={} status=no-frame", surfaceId));
      return BridgeStatus::Success;
    }
    const BridgeStatus created =
        create(BrowserObjectKind::Texture, textureId,
               std::format("acquireCurrentTexture surface={} texture={}", surfaceId, textureId));
    if (created == BridgeStatus::Success) {
      frames_[surfaceId] = textureId;
    }
    return created;
  }

  BridgeStatus abandonCurrentTexture(BrowserObjectId surfaceId) override {
    const BridgeStatus status = operate(std::format("abandonCurrentTexture surface={}", surfaceId),
                                        BrowserObjectKind::Surface, surfaceId);
    if (status != BridgeStatus::Success) {
      return status;
    }
    // The canvas owns the frame texture, so taking it back is a matter of no longer naming it.
    const auto frame = frames_.find(surfaceId);
    if (frame != frames_.end()) {
      objects->erase(frame->second);
      frames_.erase(frame);
    }
    return BridgeStatus::Success;
  }

private:
  /// A mapping the browser has been asked for.
  struct Mapping {
    MapSliceState state = MapSliceState::Pending;  //!< What a wait slice reports.
    std::vector<uint8_t> bytes;                    //!< Bytes a completed mapping exposes.
  };

  /// The object kind a bind group entry's resource alternative names.
  /// @param resource Resource alternative.
  static BrowserObjectKind KindOfBinding(BrowserBindingResource resource) {
    switch (resource) {
      case BrowserBindingResource::Buffer: return BrowserObjectKind::Buffer;
      case BrowserBindingResource::TextureView: return BrowserObjectKind::TextureView;
      case BrowserBindingResource::Sampler: return BrowserObjectKind::Sampler;
    }
    return BrowserObjectKind::Buffer;
  }

  /// Refuses a command recorded with no open encoder, mirroring the browser side, which has no
  /// encoder to record onto. @param line Recorded line naming the operation.
  BridgeStatus requireEncoder(const std::string& line) {
    if (!encoderOpen_) {
      return BridgeStatus::Failed;
    }
    return guard(line);
  }

  /// Refuses a pass command recorded with no open pass, mirroring the browser side.
  /// @param line Recorded line naming the operation.
  BridgeStatus requirePass(const std::string& line) {
    if (!encoderOpen_ || !passOpen_) {
      return BridgeStatus::Failed;
    }
    return guard(line);
  }

  /// Applies the checks every call shares: ownership, loss, and the programmed refusal.
  /// @param line Recorded line, whose leading word names the operation.
  BridgeStatus guard(const std::string& line) const {
    if (!owned) {
      return BridgeStatus::NotOwner;
    }
    if (lost) {
      return BridgeStatus::DeviceLost;
    }
    if (!failOperation.empty() && line.starts_with(failOperation)) {
      return failStatus;
    }
    return BridgeStatus::Success;
  }

  /// Refuses \p id unless it names a live object of \p kind. @param kind Expected kind.
  /// @param id Identifier to check.
  BridgeStatus require(BrowserObjectKind kind, BrowserObjectId id) const {
    const auto it = objects->find(id);
    if (it == objects->end()) {
      return BridgeStatus::UnknownObject;
    }
    if (it->second != kind) {
      return BridgeStatus::WrongObjectKind;
    }
    return BridgeStatus::Success;
  }

  /// Registers a new object of \p kind under \p id and records \p line.
  /// @param kind Kind being created. @param id Identifier to register.
  /// @param line Line to record on success.
  BridgeStatus create(BrowserObjectKind kind, BrowserObjectId id, const std::string& line) {
    if (const BridgeStatus status = guard(line); status != BridgeStatus::Success) {
      return status;
    }
    if (id == kNoBrowserObject || objects->contains(id)) {
      return BridgeStatus::Failed;
    }
    (*objects)[id] = kind;
    calls->push_back(line);
    return BridgeStatus::Success;
  }

  /// Records \p line if the shared checks pass. @param line Line to record.
  BridgeStatus operate(const std::string& line) {
    if (const BridgeStatus status = guard(line); status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back(line);
    return BridgeStatus::Success;
  }

  /// Records \p line if the shared checks pass and \p id names a live object of \p kind.
  /// @param line Line to record. @param kind Expected kind of \p id. @param id Identifier used.
  BridgeStatus operate(const std::string& line, BrowserObjectKind kind, BrowserObjectId id) {
    if (const BridgeStatus status = guard(line); status != BridgeStatus::Success) {
      return status;
    }
    if (const BridgeStatus status = require(kind, id); status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back(line);
    return BridgeStatus::Success;
  }

  /// Records a pass-scoped \p line, refusing when no pass is open. @param line Line to record.
  BridgeStatus operateInPass(const std::string& line) {
    if (const BridgeStatus status = requirePass(line); status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back(line);
    return BridgeStatus::Success;
  }

  /// Records a pass-scoped \p line naming \p id, refusing when no pass is open.
  /// @param line Line to record. @param kind Expected kind of \p id. @param id Identifier used.
  BridgeStatus operateInPass(const std::string& line, BrowserObjectKind kind, BrowserObjectId id) {
    if (const BridgeStatus status = requirePass(line); status != BridgeStatus::Success) {
      return status;
    }
    if (const BridgeStatus status = require(kind, id); status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back(line);
    return BridgeStatus::Success;
  }

  /// Records an encoder-scoped \p line naming \p id, refusing when no encoder is open.
  /// @param line Line to record. @param kind Expected kind of \p id. @param id Identifier used.
  BridgeStatus operateInEncoder(const std::string& line, BrowserObjectKind kind,
                                BrowserObjectId id) {
    if (const BridgeStatus status = requireEncoder(line); status != BridgeStatus::Success) {
      return status;
    }
    if (const BridgeStatus status = require(kind, id); status != BridgeStatus::Success) {
      return status;
    }
    calls->push_back(line);
    return BridgeStatus::Success;
  }

  std::map<BrowserObjectId, Mapping> mappings_;
  std::map<BrowserObjectId, BrowserObjectId> frames_;
  bool encoderOpen_ = false;
  bool passOpen_ = false;
};

}  // namespace donner::gpu::browser
