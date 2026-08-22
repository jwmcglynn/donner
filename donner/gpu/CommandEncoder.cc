#include "donner/gpu/CommandEncoder.h"

#include <cmath>
#include <format>
#include <sstream>
#include <utility>
#include <vector>

#include "donner/gpu/CheckedArithmetic.h"

namespace donner::gpu {

namespace {

/// Builds a \ref GpuError with the given category and message.
GpuError Err(GpuErrorType type, std::string message) {
  return GpuError{type, std::move(message)};
}

}  // namespace

Status RenderPassEncoder::setPipeline(const RenderPipeline& pipeline) {
  return encoder_->passSetPipeline(pipeline);
}

Status RenderPassEncoder::setBindGroup(uint32_t index, const BindGroup& bindGroup) {
  return encoder_->passSetBindGroup(index, bindGroup);
}

Status RenderPassEncoder::setVertexBuffer(uint32_t slot, const Buffer& buffer,
                                          uint64_t offsetBytes) {
  return encoder_->passSetVertexBuffer(slot, buffer, offsetBytes);
}

Status RenderPassEncoder::setScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  return encoder_->passSetScissorRect(x, y, width, height);
}

Status RenderPassEncoder::setViewport(float x, float y, float width, float height, float minDepth,
                                      float maxDepth) {
  return encoder_->passSetViewport(x, y, width, height, minDepth, maxDepth);
}

Status RenderPassEncoder::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                               uint32_t firstInstance) {
  return encoder_->passDraw(vertexCount, instanceCount, firstVertex, firstInstance);
}

Status RenderPassEncoder::end() {
  return encoder_->passEnd();
}

Status CommandEncoder::fail(GpuError&& error) {
  if (!firstError_) {
    firstError_ = std::move(error);
  }
  return *firstError_;
}

std::optional<GpuError> CommandEncoder::checkRecordable(bool requireActivePass) {
  if (firstError_) {
    return *firstError_;
  }
  if (finished_) {
    return Err(GpuErrorType::InvalidState, "encoder already finished");
  }
  if (requireActivePass && !inRenderPass_) {
    return Err(GpuErrorType::InvalidState, "no render pass is active");
  }
  return std::nullopt;
}

Result<RenderPassEncoder*> CommandEncoder::beginRenderPass(const RenderPassDescriptor& descriptor) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/false)) {
    return fail(std::move(*error)).error();
  }
  if (inRenderPass_) {
    return fail(Err(GpuErrorType::InvalidState, "beginRenderPass: a render pass is already active"))
        .error();
  }
  if (descriptor.colorAttachments.empty()) {
    return fail(Err(GpuErrorType::InvalidDescriptor,
                    "RenderPassDescriptor.colorAttachments is empty"))
        .error();
  }
  if (descriptor.colorAttachments.size() > kMaxColorAttachments) {
    return fail(Err(GpuErrorType::LimitExceeded,
                    std::format("RenderPassDescriptor has {} color attachments, exceeding "
                                "kMaxColorAttachments {}",
                                descriptor.colorAttachments.size(), kMaxColorAttachments)))
        .error();
  }

  Extent2d passExtent;
  std::vector<TextureFormat> attachmentFormats;
  attachmentFormats.reserve(descriptor.colorAttachments.size());
  std::vector<ResourceIdentity> seenViews;
  seenViews.reserve(descriptor.colorAttachments.size());
  for (size_t i = 0; i < descriptor.colorAttachments.size(); ++i) {
    const RenderPassColorAttachment& attachment = descriptor.colorAttachments[i];
    auto viewRecord =
        device_->resolve(device_->textureViews_, attachment.view, TextureViewTag::kName);
    if (viewRecord.hasError()) {
      return fail(std::move(viewRecord).error()).error();
    }

    const ResourceIdentity viewIdentity{attachment.view.slotIndex(), attachment.view.generation()};
    for (const ResourceIdentity& seenView : seenViews) {
      if (seenView == viewIdentity) {
        return fail(Err(GpuErrorType::InvalidDescriptor,
                        std::format("beginRenderPass: attachment {} view \"{}\" appears in "
                                    "multiple color attachments of the same pass",
                                    i, viewRecord.result()->descriptor.label.str())))
            .error();
      }
    }
    seenViews.push_back(viewIdentity);

    if (!IsKnownEnumValue(attachment.loadOp)) {
      return fail(Err(GpuErrorType::InvalidDescriptor,
                      std::format("beginRenderPass: attachment {} has an unknown loadOp value {}",
                                  i, static_cast<uint32_t>(attachment.loadOp))))
          .error();
    }
    if (!IsKnownEnumValue(attachment.storeOp)) {
      return fail(Err(GpuErrorType::InvalidDescriptor,
                      std::format("beginRenderPass: attachment {} has an unknown storeOp value {}",
                                  i, static_cast<uint32_t>(attachment.storeOp))))
          .error();
    }

    // Re-resolve the viewed texture so a view of a destroyed (or slot-reused) texture fails
    // closed here instead of aliasing another texture.
    auto viewedTexture = device_->resolveViewedTexture(*viewRecord.result());
    if (viewedTexture.hasError()) {
      return fail(std::move(viewedTexture).error()).error();
    }
    const TextureDescriptor& textureDescriptor = viewedTexture.result()->descriptor;
    attachmentFormats.push_back(textureDescriptor.format);

    if (!HasAllFlags(textureDescriptor.usage, TextureUsage::RenderAttachment)) {
      return fail(Err(GpuErrorType::UsageMismatch,
                      std::format("beginRenderPass: attachment {} view \"{}\" lacks the "
                                  "RenderAttachment usage",
                                  i, viewRecord.result()->descriptor.label.str())))
          .error();
    }
    for (double channel : attachment.clearColor) {
      if (!std::isfinite(channel)) {
        return fail(Err(GpuErrorType::InvalidDescriptor,
                        std::format("beginRenderPass: attachment {} clearColor is not finite", i)))
            .error();
      }
    }
    if (i == 0) {
      passExtent = textureDescriptor.size;
    } else if (!(textureDescriptor.size == passExtent)) {
      return fail(Err(GpuErrorType::InvalidDescriptor,
                      std::format("beginRenderPass: attachment {} extent {}x{} differs from "
                                  "attachment 0 extent {}x{}",
                                  i, textureDescriptor.size.width, textureDescriptor.size.height,
                                  passExtent.width, passExtent.height)))
          .error();
    }
  }

  inRenderPass_ = true;
  passExtent_ = passExtent;
  passAttachmentFormats_ = std::move(attachmentFormats);
  currentPipeline_.reset();
  boundVertexBuffers_.fill(std::nullopt);
  boundBindGroups_.fill(std::nullopt);
  commands_.push_back(BeginRenderPassCommand{descriptor});
  return &passEncoder_;
}

Status CommandEncoder::passSetPipeline(const RenderPipeline& pipeline) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }
  auto record = device_->resolve(device_->renderPipelines_, pipeline, RenderPipelineTag::kName);
  if (record.hasError()) {
    return fail(std::move(record).error());
  }

  // The pipeline's color targets must match the active pass's attachments in count and
  // per-index format. InvalidState (not UsageMismatch) by convention: like the draw-time bind
  // group / pipeline layout check, this is an incompatibility between a valid object and the
  // encoder's current state, whereas UsageMismatch is reserved for resources missing a usage
  // flag.
  const std::vector<ColorTargetState>& targets = record.result()->descriptor.fragment.targets;
  if (targets.size() != passAttachmentFormats_.size()) {
    return fail(
        Err(GpuErrorType::InvalidState,
            std::format("setPipeline: pipeline declares {} color targets but the pass has {} "
                        "attachments",
                        targets.size(), passAttachmentFormats_.size())));
  }
  for (size_t i = 0; i < targets.size(); ++i) {
    if (targets[i].format != passAttachmentFormats_[i]) {
      std::ostringstream formats;
      formats << targets[i].format << " vs " << passAttachmentFormats_[i];
      return fail(Err(GpuErrorType::InvalidState,
                      std::format("setPipeline: color target {} format does not match the pass "
                                  "attachment format ({})",
                                  i, formats.str())));
    }
  }

  currentPipeline_ = BoundPipeline{record.result()->descriptor.vertex.buffers,
                                   record.result()->bindGroupLayoutIds};
  commands_.push_back(
      SetPipelineCommand{ResourceIdentity{pipeline.slotIndex(), pipeline.generation()}});
  return OkStatus();
}

Status CommandEncoder::passSetBindGroup(uint32_t index, const BindGroup& bindGroup) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }
  if (index >= kMaxBindGroups) {
    return fail(Err(
        GpuErrorType::LimitExceeded,
        std::format("setBindGroup: index {} exceeds kMaxBindGroups {}", index, kMaxBindGroups)));
  }
  auto record = device_->resolve(device_->bindGroups_, bindGroup, BindGroupTag::kName);
  if (record.hasError()) {
    return fail(std::move(record).error());
  }

  // Re-resolve everything the group references: the layout it was created against (backends
  // read it at encode time) plus every entry buffer, texture view (and its texture), and
  // sampler. A dependency destroyed after createBindGroup - including slot reuse - must fail
  // closed here instead of reaching a backend.
  const Device::BindGroupLayoutRecord* layoutRecord = device_->bindGroupLayouts_.find(
      record.result()->layoutIdentity.slotIndex, record.result()->layoutIdentity.generation);
  if (layoutRecord == nullptr) {
    return fail(Err(GpuErrorType::InvalidHandle,
                    std::format("setBindGroup: bind group \"{}\" is stale; the layout it was "
                                "created against was destroyed (layout slot {})",
                                record.result()->descriptor.label.str(),
                                record.result()->layoutIdentity.slotIndex)));
  }
  for (const BindGroupEntry& entry : record.result()->descriptor.entries) {
    if (const BufferBinding* bufferBinding = std::get_if<BufferBinding>(&entry.resource)) {
      auto bufferRecord =
          device_->resolve(device_->buffers_, bufferBinding->buffer, BufferTag::kName);
      if (bufferRecord.hasError()) {
        return fail(std::move(bufferRecord).error());
      }
    } else if (const TextureViewBinding* viewBinding =
                   std::get_if<TextureViewBinding>(&entry.resource)) {
      auto viewRecord =
          device_->resolve(device_->textureViews_, viewBinding->view, TextureViewTag::kName);
      if (viewRecord.hasError()) {
        return fail(std::move(viewRecord).error());
      }
      auto viewedTexture = device_->resolveViewedTexture(*viewRecord.result());
      if (viewedTexture.hasError()) {
        return fail(std::move(viewedTexture).error());
      }
    } else if (const SamplerBinding* samplerBinding =
                   std::get_if<SamplerBinding>(&entry.resource)) {
      auto samplerRecord =
          device_->resolve(device_->samplers_, samplerBinding->sampler, SamplerTag::kName);
      if (samplerRecord.hasError()) {
        return fail(std::move(samplerRecord).error());
      }
    }
  }

  boundBindGroups_[index] = BoundBindGroup{bindGroup.slotIndex(), record.result()->layoutIdentity};
  commands_.push_back(
      SetBindGroupCommand{index, ResourceIdentity{bindGroup.slotIndex(), bindGroup.generation()}});
  return OkStatus();
}

Status CommandEncoder::passSetVertexBuffer(uint32_t slot, const Buffer& buffer,
                                           uint64_t offsetBytes) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }
  if (slot >= kMaxVertexBuffers) {
    return fail(Err(GpuErrorType::LimitExceeded,
                    std::format("setVertexBuffer: slot {} exceeds kMaxVertexBuffers {}", slot,
                                kMaxVertexBuffers)));
  }
  auto record = device_->resolve(device_->buffers_, buffer, BufferTag::kName);
  if (record.hasError()) {
    return fail(std::move(record).error());
  }
  if (!HasAllFlags(record.result()->descriptor.usage, BufferUsage::Vertex)) {
    return fail(Err(GpuErrorType::UsageMismatch,
                    std::format("setVertexBuffer: buffer \"{}\" lacks the Vertex usage",
                                record.result()->descriptor.label.str())));
  }
  if (offsetBytes > record.result()->descriptor.byteSize) {
    return fail(Err(GpuErrorType::OutOfBounds,
                    std::format("setVertexBuffer: offsetBytes {} exceeds buffer \"{}\" size {}",
                                offsetBytes, record.result()->descriptor.label.str(),
                                record.result()->descriptor.byteSize)));
  }

  boundVertexBuffers_[slot] =
      BoundVertexBuffer{buffer.slotIndex(), record.result()->descriptor.byteSize - offsetBytes};
  commands_.push_back(SetVertexBufferCommand{
      slot, ResourceIdentity{buffer.slotIndex(), buffer.generation()}, offsetBytes});
  return OkStatus();
}

Status CommandEncoder::passSetScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }
  const std::optional<uint64_t> right = CheckedAdd(x, width);
  const std::optional<uint64_t> bottom = CheckedAdd(y, height);
  if (!right || *right > passExtent_.width || !bottom || *bottom > passExtent_.height) {
    return fail(Err(GpuErrorType::OutOfBounds,
                    std::format("setScissorRect: rect x={} y={} width={} height={} exceeds pass "
                                "extent {}x{}",
                                x, y, width, height, passExtent_.width, passExtent_.height)));
  }

  commands_.push_back(SetScissorRectCommand{x, y, width, height});
  return OkStatus();
}

Status CommandEncoder::passSetViewport(float x, float y, float width, float height, float minDepth,
                                       float maxDepth) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }
  const bool allFinite = std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
                         std::isfinite(height) && std::isfinite(minDepth) &&
                         std::isfinite(maxDepth);
  if (!allFinite || width <= 0 || height <= 0 || minDepth < 0 || maxDepth > 1 ||
      minDepth > maxDepth) {
    return fail(Err(GpuErrorType::InvalidDescriptor,
                    std::format("setViewport: invalid viewport x={} y={} width={} height={} "
                                "minDepth={} maxDepth={}",
                                x, y, width, height, minDepth, maxDepth)));
  }

  commands_.push_back(SetViewportCommand{x, y, width, height, minDepth, maxDepth});
  return OkStatus();
}

Status CommandEncoder::passDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                                uint32_t firstInstance) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }
  if (!currentPipeline_) {
    return fail(Err(GpuErrorType::InvalidState, "draw: no pipeline is set"));
  }

  for (size_t slot = 0; slot < currentPipeline_->vertexBuffers.size(); ++slot) {
    const VertexBufferLayout& layout = currentPipeline_->vertexBuffers[slot];
    if (!boundVertexBuffers_[slot]) {
      return fail(Err(GpuErrorType::InvalidState,
                      std::format("draw: the pipeline requires a vertex buffer at slot {} but "
                                  "none is bound",
                                  slot)));
    }
    const bool perVertex = layout.stepMode == VertexStepMode::Vertex;
    const uint64_t first = perVertex ? firstVertex : firstInstance;
    const uint64_t count = perVertex ? vertexCount : instanceCount;
    const std::optional<uint64_t> lastElement = CheckedAdd(first, count);
    const std::optional<uint64_t> bytesNeeded =
        lastElement ? CheckedMul(*lastElement, layout.strideBytes) : std::nullopt;
    if (!bytesNeeded || *bytesNeeded > boundVertexBuffers_[slot]->bytesAvailable) {
      return fail(
          Err(GpuErrorType::OutOfBounds,
              std::format("draw: {} range [{}, {}) with strideBytes {} overflows the "
                          "vertex buffer bound at slot {} ({} bytes available)",
                          perVertex ? "vertex" : "instance", first, lastElement ? *lastElement : 0,
                          layout.strideBytes, slot, boundVertexBuffers_[slot]->bytesAvailable)));
    }
  }

  for (size_t index = 0; index < currentPipeline_->bindGroupLayoutIds.size(); ++index) {
    if (!boundBindGroups_[index]) {
      return fail(Err(GpuErrorType::InvalidState,
                      std::format("draw: the pipeline layout requires a bind group at index {} "
                                  "but none is bound",
                                  index)));
    }
    if (!(boundBindGroups_[index]->layoutIdentity == currentPipeline_->bindGroupLayoutIds[index])) {
      return fail(Err(GpuErrorType::InvalidState,
                      std::format("draw: the bind group at index {} was created against a "
                                  "different layout than the pipeline expects",
                                  index)));
    }
  }

  commands_.push_back(DrawCommand{vertexCount, instanceCount, firstVertex, firstInstance});
  return OkStatus();
}

Status CommandEncoder::passEnd() {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/true)) {
    return fail(std::move(*error));
  }

  inRenderPass_ = false;
  currentPipeline_.reset();
  boundVertexBuffers_.fill(std::nullopt);
  boundBindGroups_.fill(std::nullopt);
  commands_.push_back(EndRenderPassCommand{});
  return OkStatus();
}

Status CommandEncoder::copyTextureToBuffer(const TexelCopyTextureInfo& source,
                                           const Buffer& destination,
                                           const TexelCopyBufferLayout& destinationLayout,
                                           const Extent2d& copySize) {
  if (std::optional<GpuError> error = checkRecordable(/*requireActivePass=*/false)) {
    return fail(std::move(*error));
  }
  if (inRenderPass_) {
    return fail(
        Err(GpuErrorType::InvalidState, "copyTextureToBuffer: not allowed inside a render pass"));
  }
  auto textureRecord = device_->resolve(device_->textures_, source.texture, TextureTag::kName);
  if (textureRecord.hasError()) {
    return fail(std::move(textureRecord).error());
  }
  auto bufferRecord = device_->resolve(device_->buffers_, destination, BufferTag::kName);
  if (bufferRecord.hasError()) {
    return fail(std::move(bufferRecord).error());
  }
  const TextureDescriptor& textureDescriptor = textureRecord.result()->descriptor;
  const BufferDescriptor& bufferDescriptor = bufferRecord.result()->descriptor;
  if (!HasAllFlags(textureDescriptor.usage, TextureUsage::CopySrc)) {
    return fail(Err(GpuErrorType::UsageMismatch,
                    std::format("copyTextureToBuffer: texture \"{}\" lacks the CopySrc usage",
                                textureDescriptor.label.str())));
  }
  if (!HasAllFlags(bufferDescriptor.usage, BufferUsage::CopyDst)) {
    return fail(Err(GpuErrorType::UsageMismatch,
                    std::format("copyTextureToBuffer: buffer \"{}\" lacks the CopyDst usage",
                                bufferDescriptor.label.str())));
  }
  if (copySize.width > textureDescriptor.size.width ||
      copySize.height > textureDescriptor.size.height) {
    return fail(
        Err(GpuErrorType::OutOfBounds,
            std::format("copyTextureToBuffer: copy size {}x{} exceeds texture \"{}\" size {}x{}",
                        copySize.width, copySize.height, textureDescriptor.label.str(),
                        textureDescriptor.size.width, textureDescriptor.size.height)));
  }
  Result<uint64_t> requiredEnd = ValidateTexelCopyInternal(
      destinationLayout, copySize, textureDescriptor.format, "copyTextureToBuffer");
  if (requiredEnd.hasError()) {
    return fail(std::move(requiredEnd).error());
  }
  if (requiredEnd.result() > bufferDescriptor.byteSize) {
    return fail(Err(GpuErrorType::OutOfBounds,
                    std::format("copyTextureToBuffer: layout requires {} bytes but buffer \"{}\" "
                                "has {} bytes",
                                requiredEnd.result(), bufferDescriptor.label.str(),
                                bufferDescriptor.byteSize)));
  }

  commands_.push_back(CopyTextureToBufferCommand{
      ResourceIdentity{source.texture.slotIndex(), source.texture.generation()},
      ResourceIdentity{destination.slotIndex(), destination.generation()}, destinationLayout,
      copySize});
  return OkStatus();
}

Result<CommandBuffer> CommandEncoder::finish() {
  if (firstError_) {
    return *firstError_;
  }
  if (finished_) {
    return Err(GpuErrorType::InvalidState, "finish: encoder already finished");
  }
  if (inRenderPass_) {
    return fail(Err(GpuErrorType::InvalidState, "finish: a render pass is still active")).error();
  }

  finished_ = true;
  return device_->registerCommandBuffer(std::move(commands_));
}

}  // namespace donner::gpu
