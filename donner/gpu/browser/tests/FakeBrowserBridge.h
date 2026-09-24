#pragma once
/// @file
/// \c donner::gpu::browser::FakeBrowserBridge - a browser side without a browser, for tests.

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/gpu/browser/BrowserBridge.h"
#include "donner/gpu/browser/BrowserWireCodes.h"

namespace donner::gpu::browser {

/**
 * The browser GPU device several fake bridges share, as the logical devices of one worker do.
 *
 * It holds what belongs to the device rather than to any one logical device over it: whether the
 * device is lost, and the textures it has allocated, each with whether it has been destroyed. A
 * bridge keeps identifiers of its own and names these textures through them, so a test can see
 * that two logical devices name one texture, and exactly when that texture goes away.
 */
class FakeBrowserGpuDevice {
public:
  /// Whether the browser has reported this device lost, for every logical device over it.
  bool lost = false;

  /// Allocates a texture and returns the number naming it on this device.
  uint64_t allocateTexture() {
    const uint64_t texture = nextTexture_++;
    textures_[texture] = false;
    return texture;
  }

  /// Destroys the texture \p texture names. @param texture Texture to destroy.
  void destroyTexture(uint64_t texture) { textures_[texture] = true; }

  /// Whether \p texture names a texture this device allocated and has not destroyed.
  /// @param texture Texture to check.
  [[nodiscard]] bool isTextureLive(uint64_t texture) const {
    const auto it = textures_.find(texture);
    return it != textures_.end() && !it->second;
  }

  /// Holds \p texture for the other logical devices and returns the share naming the hold.
  /// @param texture Texture to hold.
  /// @param canvasOwned Whether the texture is a canvas frame, which releasing the share never
  ///   destroys because the canvas owns it.
  BrowserTextureShareId holdTexture(uint64_t texture, bool canvasOwned = false) {
    const BrowserTextureShareId share = nextShare_++;
    shares_[share] =
        Share{.texture = texture, .producerReleased = false, .canvasOwned = canvasOwned};
    return share;
  }

  /// The texture \p share holds, or nullopt once the share is released.
  /// @param share Share to look up.
  [[nodiscard]] std::optional<uint64_t> sharedTexture(BrowserTextureShareId share) const {
    const auto it = shares_.find(share);
    if (it == shares_.end()) {
      return std::nullopt;
    }
    return it->second.texture;
  }

  /// Records that the producer released its identifier for the texture \p share holds, which is
  /// then destroyed when the share is released rather than now.
  /// @param share Share whose producer let go.
  void releaseProducer(BrowserTextureShareId share) { shares_[share].producerReleased = true; }

  /// Releases \p share, destroying its texture if the producer has already let go of it and the
  /// texture is not a canvas frame. @param share Share to release.
  void releaseShare(BrowserTextureShareId share) {
    const auto it = shares_.find(share);
    if (it == shares_.end()) {
      return;
    }
    if (it->second.producerReleased && !it->second.canvasOwned) {
      destroyTexture(it->second.texture);
    }
    shares_.erase(it);
    ++releasedShares;
  }

  /// How many shares have been released.
  uint64_t releasedShares = 0;

  /// Lets the device go, as the browser side does once no logical device over it is left: every
  /// texture a share still holds is destroyed, except a canvas frame, and the shares are
  /// forgotten, so a release that arrives afterwards finds nothing.
  void release() {
    for (const auto& [share, held] : shares_) {
      if (!held.canvasOwned) {
        destroyTexture(held.texture);
      }
    }
    shares_.clear();
  }

  /// Number of shares the device still holds.
  [[nodiscard]] size_t liveShares() const { return shares_.size(); }

private:
  /// A texture held for the other logical devices.
  struct Share {
    uint64_t texture = 0;           //!< Texture held.
    bool producerReleased = false;  //!< Whether the producer has let go of its identifier.
    bool canvasOwned = false;       //!< Whether the texture is a canvas frame.
  };

  uint64_t nextTexture_ = 1;
  std::map<uint64_t, bool> textures_;  //!< Texture number to whether it was destroyed.
  BrowserTextureShareId nextShare_ = 1;
  std::map<BrowserTextureShareId, Share> shares_;
};

/// A share the fake browser device made, released on that device when the runtime drops it.
///
/// Like the browser side's, it belongs to the worker that made it: a drop on the thread that made
/// the share releases it there, and a drop on any other thread does not reach the device.
class FakeSharedTexture final : public BrowserSharedTexture {
public:
  /// Constructs the share \p share of \p gpuDevice. @param gpuDevice Device the share belongs to.
  /// @param share Share identifier.
  FakeSharedTexture(std::shared_ptr<FakeBrowserGpuDevice> gpuDevice, BrowserTextureShareId share)
      : gpuDevice_(std::move(gpuDevice)), share_(share), ownerThread_(std::this_thread::get_id()) {}

  /// Destructor; releases the share on its device when dropped on the thread that made it.
  ~FakeSharedTexture() override {
    if (std::this_thread::get_id() == ownerThread_) {
      gpuDevice_->releaseShare(share_);
    }
  }

  BrowserTextureShareId shareId() const override { return share_; }
  const void* sharedDeviceIdentity() const override { return gpuDevice_.get(); }

private:
  std::shared_ptr<FakeBrowserGpuDevice> gpuDevice_;
  BrowserTextureShareId share_;
  std::thread::id ownerThread_;
};

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
  /// Constructs a bridge whose device request is already settled and ready, over a browser
  /// device of its own.
  FakeBrowserBridge() = default;

  /// Constructs a bridge over \p gpuDevice, as a further logical device of the worker that holds
  /// it. @param gpuDevice Browser device the bridge shares with the others over it.
  explicit FakeBrowserBridge(std::shared_ptr<FakeBrowserGpuDevice> gpuDevice)
      : gpuDevice(std::move(gpuDevice)) {}

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
  /// Whether the browser has reported the device lost to this logical device. The shared device's
  /// own \ref FakeBrowserGpuDevice::lost reports it to every logical device over it.
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

  /// The browser device this bridge's logical device runs on, which other bridges may share.
  std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = std::make_shared<FakeBrowserGpuDevice>();

  /// The device texture \p textureId names, or nullopt when it names no texture here.
  /// @param textureId Identifier in this bridge's own space.
  std::optional<uint64_t> nativeTextureOf(BrowserObjectId textureId) const {
    const auto it = nativeTextures_.find(textureId);
    if (it == nativeTextures_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

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

  /// The texels the texture \p textureId currently holds, tightly packed row by row with no row
  /// padding, or an empty vector for a texture this bridge holds no image for.
  ///
  /// The image is what the accepted writes put there, so a test reads back the placement the
  /// backend asked for rather than only the line it recorded.
  /// @param textureId Texture to read.
  std::vector<uint8_t> textureTexels(BrowserObjectId textureId) const {
    const auto it = textureImages_.find(textureId);
    if (it == textureImages_.end()) {
      return {};
    }
    return it->second.texels;
  }

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

  bool isDeviceLost() const override { return lost || gpuDevice->lost; }

  RcString deviceLostReason() const override { return lostReason; }

  uint64_t completedSerial() const override { return completed; }

  const void* sharedDeviceIdentity() const override {
    return requestState == BrowserDeviceRequestState::Ready ? gpuDevice.get() : nullptr;
  }

  BridgeStatus shareTexture(BrowserObjectId textureId,
                            std::shared_ptr<const BrowserSharedTexture>& shared) override {
    const std::string line = std::format("shareTexture texture={}", textureId);
    if (const BridgeStatus status = guard(line); status != BridgeStatus::Success) {
      return status;
    }
    if (const BridgeStatus status = require(BrowserObjectKind::Texture, textureId);
        status != BridgeStatus::Success) {
      return status;
    }
    const auto native = nativeTextures_.find(textureId);
    // A registration is shared from the device that allocated it, and a texture is shared once, as
    // on the browser side. A frame is shared too, and stays its canvas's.
    if (native == nativeTextures_.end() || aliases_.contains(textureId) ||
        sharedAs_.contains(textureId)) {
      return BridgeStatus::Failed;
    }
    const BrowserTextureShareId share =
        gpuDevice->holdTexture(native->second, /*canvasOwned=*/isFrame(textureId));
    sharedAs_[textureId] = share;
    calls->push_back(std::format("{} share={}", line, share));
    shared = std::make_shared<const FakeSharedTexture>(gpuDevice, share);
    return BridgeStatus::Success;
  }

  BridgeStatus registerSharedTexture(BrowserObjectId id,
                                     const BrowserSharedTexture& shared) override {
    const std::string line =
        std::format("registerSharedTexture id={} share={}", id, shared.shareId());
    // Ownership and loss come first, as on the browser side, so a lost device reports its loss
    // rather than whether the share still exists.
    if (const BridgeStatus status = guard(line); status != BridgeStatus::Success) {
      return status;
    }
    const std::optional<uint64_t> native = gpuDevice->sharedTexture(shared.shareId());
    if (!native.has_value()) {
      return BridgeStatus::UnknownObject;
    }
    const BridgeStatus status = create(BrowserObjectKind::Texture, id, line);
    if (status == BridgeStatus::Success) {
      nativeTextures_[id] = *native;
      aliases_.insert(id);
    }
    return status;
  }

  BridgeStatus createBuffer(BrowserObjectId id, uint64_t byteSize, uint32_t usageBits) override {
    return create(BrowserObjectKind::Buffer, id,
                  std::format("createBuffer id={} byteSize={} usage={}", id, byteSize, usageBits));
  }

  BridgeStatus createTexture(BrowserObjectId id, uint32_t width, uint32_t height,
                             uint32_t formatCode, uint32_t usageBits) override {
    const BridgeStatus status =
        create(BrowserObjectKind::Texture, id,
               std::format("createTexture id={} size={}x{} format={} usage={}", id, width, height,
                           formatCode, usageBits));
    if (status == BridgeStatus::Success) {
      nativeTextures_[id] = gpuDevice->allocateTexture();
      if (const uint32_t texelBytes = BytesPerTexel(formatCode); texelBytes != 0) {
        textureImages_[id] =
            TextureImage{width, height, texelBytes,
                         std::vector<uint8_t>(size_t{width} * height * texelBytes, 0)};
      }
    }
    return status;
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
    const std::string line =
        std::format("destroyObject kind={} id={}", BrowserObjectKindName(kind), id);
    if (const BridgeStatus status = release(line, kind, id); status != BridgeStatus::Success) {
      return status;
    }
    objects->erase(id);
    mappings_.erase(id);
    textureImages_.erase(id);
    if (const auto native = nativeTextures_.find(id); native != nativeTextures_.end()) {
      // An alias names another logical device's texture, and a texture a share still holds goes
      // when the share does; only a texture no one else holds is destroyed here.
      const bool alias = aliases_.erase(id) != 0;
      const auto shared = sharedAs_.find(id);
      const bool held =
          shared != sharedAs_.end() && gpuDevice->sharedTexture(shared->second).has_value();
      if (held) {
        gpuDevice->releaseProducer(shared->second);
      } else if (!alias) {
        gpuDevice->destroyTexture(native->second);
      }
      if (shared != sharedAs_.end()) {
        sharedAs_.erase(shared);
      }
      nativeTextures_.erase(native);
    }
    if (kind == BrowserObjectKind::Surface) {
      // A surface destroyed while it still names a frame gives that frame up with it: the canvas
      // owns the texture, so nothing is left to name it once its surface is gone.
      releaseFrame(id);
    }
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
    const BridgeStatus status = operate(
        std::format("writeTexture texture={} bytes={} offset={} bytesPerRow={} rowsPerImage={} "
                    "destination=({},{}) size={}x{}",
                    textureId, data.size(), layout.offsetBytes, layout.bytesPerRow,
                    layout.rowsPerImage, region.destinationX, region.destinationY, region.width,
                    region.height),
        BrowserObjectKind::Texture, textureId);
    if (status == BridgeStatus::Success) {
      applyTextureWrite(textureId, data, layout, region);
    }
    return status;
  }

  BridgeStatus beginCommandBuffer(uint64_t submissionSerial, uint32_t commandBufferIndex) override {
    // A later buffer must continue the submission the first one opened, and both halves must
    // agree on how many buffers it has finished, as on the browser side.
    if (commandBufferIndex != 0 &&
        (recordingSerial_ != submissionSerial || finishedCommandBuffers_ != commandBufferIndex)) {
      return BridgeStatus::Failed;
    }
    const BridgeStatus status = operate(
        std::format("beginCommandBuffer serial={} index={}", submissionSerial, commandBufferIndex));
    if (status == BridgeStatus::Success) {
      // A recording left open by a submission that was refused partway is discarded here rather
      // than continued, so nothing recorded before the refusal can reach the queue.
      encoderOpen_ = true;
      passOpen_ = false;
      recordingSerial_ = submissionSerial;
      if (commandBufferIndex == 0) {
        finishedCommandBuffers_ = 0;
      }
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
    // The serial that opened the recording is the one that must close it, as on the browser side.
    if (passOpen_ || recordingSerial_ != submissionSerial) {
      return BridgeStatus::Failed;
    }
    calls->push_back(line);
    encoderOpen_ = false;
    ++finishedCommandBuffers_;
    return BridgeStatus::Success;
  }

  BridgeStatus submitCommandBuffers(uint64_t submissionSerial) override {
    // Every buffer of the submission must be finished and belong to this serial, and a
    // submission with no buffers names no work, as on the browser side.
    if (encoderOpen_ || recordingSerial_ != submissionSerial || finishedCommandBuffers_ == 0) {
      return BridgeStatus::Failed;
    }
    const BridgeStatus status = operate(std::format("submitCommandBuffers serial={} count={}",
                                                    submissionSerial, finishedCommandBuffers_));
    if (status == BridgeStatus::Success) {
      finishedCommandBuffers_ = 0;
      recordingSerial_ = 0;
    }
    return status;
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

  /// How many times the device was given the browser's thread, and for how long in total. A test
  /// asserts a pending wait yields rather than spinning, which is the difference between a mapping
  /// that can complete and one that cannot.
  mutable uint64_t yieldCount = 0;
  /// Total seconds handed over through \ref yieldToBrowser.
  mutable double yieldedSeconds = 0.0;

  void yieldToBrowser(double seconds) override {
    ++yieldCount;
    yieldedSeconds += seconds;
    // A browser would settle promises here. The fake stands in for that by letting a test arrange
    // what the next state is before the wait looks again.
    if (onYield) {
      onYield();
    }
  }

  /// Run when the device yields, so a test can complete a mapping the way a browser would.
  std::function<void()> onYield;

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
    const BridgeStatus status =
        operate(std::format("configureSurface surface={} format={} usage={} size={}x{} "
                            "alphaMode={}",
                            surfaceId, formatCode, usageBits, width, height, alphaModeCode),
                BrowserObjectKind::Surface, surfaceId);
    if (status == BridgeStatus::Success) {
      // Configuring replaces the swap chain behind the context, so the browser side drops the
      // frame it was holding rather than carrying it across.
      releaseFrame(surfaceId);
    }
    return status;
  }

  BridgeStatus acquireCurrentTexture(BrowserObjectId surfaceId, BrowserObjectId textureId,
                                     SurfaceStatus& status) override {
    if (const BridgeStatus bridgeStatus = require(BrowserObjectKind::Surface, surfaceId);
        bridgeStatus != BridgeStatus::Success) {
      return bridgeStatus;
    }
    if (frames_.find(surfaceId) != frames_.end()) {
      // A canvas holds one frame at a time and refuses a second while the first is still named,
      // as the browser side does: taking another would leave nothing able to name the first.
      return BridgeStatus::Failed;
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
      // The canvas's texture, which the browser device can share like any other but which nothing
      // here destroys.
      nativeTextures_[textureId] = gpuDevice->allocateTexture();
    }
    return created;
  }

  BridgeStatus abandonCurrentTexture(BrowserObjectId surfaceId) override {
    // Handing a frame back is a release, so a lost device takes it: the browser side refuses work
    // on one but not releases, and refusing here would leave the canvas holding a frame for a
    // device that can never draw another.
    const BridgeStatus status = release(std::format("abandonCurrentTexture surface={}", surfaceId),
                                        BrowserObjectKind::Surface, surfaceId);
    if (status != BridgeStatus::Success) {
      return status;
    }
    releaseFrame(surfaceId);
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
    if (isDeviceLost()) {
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

  /// Stops naming the frame the surface \p surfaceId took from its canvas, if it has one.
  ///
  /// The canvas owns the frame texture, so taking it back is a matter of no longer naming it.
  /// @param surfaceId Surface holding the frame.
  void releaseFrame(BrowserObjectId surfaceId) {
    const auto frame = frames_.find(surfaceId);
    if (frame != frames_.end()) {
      objects->erase(frame->second);
      nativeTextures_.erase(frame->second);
      if (const auto shared = sharedAs_.find(frame->second); shared != sharedAs_.end()) {
        // A share of the frame outlives the surface taking it back, and still never destroys it.
        if (gpuDevice->sharedTexture(shared->second).has_value()) {
          gpuDevice->releaseProducer(shared->second);
        }
        sharedAs_.erase(shared);
      }
      frames_.erase(frame);
    }
  }

  /// Whether \p textureId names a frame some surface has out. @param textureId Texture to check.
  [[nodiscard]] bool isFrame(BrowserObjectId textureId) const {
    for (const auto& [surfaceId, frameId] : frames_) {
      if (frameId == textureId) {
        return true;
      }
    }
    return false;
  }

  /// Records \p line for a release naming \p id of \p kind.
  ///
  /// A release takes every check an operation does except loss: a lost device still has to free
  /// what it holds, and the browser side accepts releases on one for that reason.
  /// @param line Line to record. @param kind Expected kind of \p id. @param id Identifier used.
  BridgeStatus release(const std::string& line, BrowserObjectKind kind, BrowserObjectId id) {
    if (!owned) {
      return BridgeStatus::NotOwner;
    }
    if (!failOperation.empty() && line.starts_with(failOperation)) {
      return failStatus;
    }
    if (const BridgeStatus status = require(kind, id); status != BridgeStatus::Success) {
      return status;
    }
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

  /// A texture's contents, as this bridge models them.
  struct TextureImage {
    uint32_t width = 0;           //!< Width in texels.
    uint32_t height = 0;          //!< Height in texels.
    uint32_t bytesPerTexel = 0;   //!< Bytes one texel occupies.
    std::vector<uint8_t> texels;  //!< Tightly packed texels, row by row.
  };

  /// Bytes one texel of the wire format \p formatCode occupies, or zero for a code that names no
  /// format, in which case no image is modelled for the texture.
  /// @param formatCode Encoded \ref TextureFormat from the creation call.
  static uint32_t BytesPerTexel(uint32_t formatCode) {
    static constexpr std::pair<TextureFormat, uint32_t> kFormatSizes[] = {
        {TextureFormat::RGBA8Unorm, 4},
        {TextureFormat::BGRA8Unorm, 4},
        {TextureFormat::R8Unorm, 1},
        {TextureFormat::RGBA32Float, 16}};
    for (const auto& [format, bytes] : kFormatSizes) {
      if (WireTextureFormat(format) == formatCode) {
        return bytes;
      }
    }
    return 0;
  }

  /// Copies the rectangle \p region of \p data into the image of \p textureId, reading rows
  /// through \p layout.
  ///
  /// The runtime validated the rectangle against the destination before the backend was called,
  /// so a texel that lands outside the image means the backend misplaced the rectangle; it is
  /// dropped here, which leaves that destination texel at whatever it held and shows up as a
  /// difference rather than as an out-of-bounds write.
  /// @param textureId Destination texture. @param data Payload bytes.
  /// @param layout Row layout of \p data. @param region Rectangle being written.
  void applyTextureWrite(BrowserObjectId textureId, std::span<const uint8_t> data,
                         const BrowserTexelLayout& layout, const BrowserCopyRegion& region) {
    const auto it = textureImages_.find(textureId);
    if (it == textureImages_.end()) {
      return;
    }
    TextureImage& image = it->second;
    const size_t texelBytes = image.bytesPerTexel;
    for (uint32_t row = 0; row < region.height; ++row) {
      for (uint32_t column = 0; column < region.width; ++column) {
        const uint32_t x = region.destinationX + column;
        const uint32_t y = region.destinationY + row;
        if (x >= image.width || y >= image.height) {
          continue;
        }
        const size_t source = static_cast<size_t>(layout.offsetBytes) +
                              size_t{row} * layout.bytesPerRow + size_t{column} * texelBytes;
        if (source + texelBytes > data.size()) {
          return;
        }
        const size_t destination = (size_t{y} * image.width + x) * texelBytes;
        std::copy_n(data.begin() + static_cast<ptrdiff_t>(source), texelBytes,
                    image.texels.begin() + static_cast<ptrdiff_t>(destination));
      }
    }
  }

  std::map<BrowserObjectId, Mapping> mappings_;
  std::map<BrowserObjectId, TextureImage> textureImages_;
  std::map<BrowserObjectId, BrowserObjectId> frames_;
  /// Device texture each texture identifier of this bridge names.
  std::map<BrowserObjectId, uint64_t> nativeTextures_;
  /// Texture identifiers of this bridge that are registrations of another logical device's texture.
  std::set<BrowserObjectId> aliases_;
  /// Share each texture this bridge shared was held under.
  std::map<BrowserObjectId, BrowserTextureShareId> sharedAs_;
  bool encoderOpen_ = false;
  bool passOpen_ = false;
  uint64_t recordingSerial_ = 0;
  uint32_t finishedCommandBuffers_ = 0;  //!< Buffers finished under \ref recordingSerial_.
};

}  // namespace donner::gpu::browser
