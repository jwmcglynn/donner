#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

namespace {

/// The device-owned pipeline for @p blendMode. Each blend mode is compiled and
/// cached independently, so a consumer only pays for the variant it draws.
GeodeCheckerboardPipeline& PipelineForBlendMode(GeodeDevice& device,
                                                GeodeCheckerboardPipeline::BlendMode blendMode) {
  return blendMode == GeodeCheckerboardPipeline::BlendMode::DestinationOver
             ? device.checkerboardUnderlayPipeline()
             : device.checkerboardPipeline();
}

/// True when the requested pass has a target to draw into and an appearance that produces
/// visible cells. A degenerate request is not an error; the caller simply draws nothing.
bool CheckerboardRequestIsDrawable(const gpu::Texture& target, Vector2i targetSizePx,
                                   const CheckerboardUnderlayParams& params) {
  if (!target.isValid() || targetSizePx.x <= 0 || targetSizePx.y <= 0) {
    return false;
  }
  if (!(params.devicePixelRatio > 0.0) || !(params.cellSizeLogicalPx > 0.0)) {
    return false;
  }
  return !params.scissorPx.has_value() ||
         (params.scissorPx->width != 0 && params.scissorPx->height != 0);
}

/// Validated recording state for one target. The view and encoder retain every runtime handle
/// needed by the borrowed attachment until the command buffer is finished.
struct PreparedTargetPass {
  gpu::TextureView targetView;
  std::unique_ptr<gpu::CommandEncoder> encoder;
  gpu::RenderPassEncoder* renderPass = nullptr;
  const GeodeCheckerboardPipeline* pipeline = nullptr;
};

/// Validate the borrowed target and begin its pass before per-consumer resources are allocated.
std::optional<PreparedTargetPass> PrepareTargetPass(
    GeodeDevice& device, const gpu::Texture& target,
    GeodeCheckerboardPipeline::BlendMode blendMode) {
  GeodeWgpuAdapterDevice& adapterDevice = device.adapterDevice();
  gpu::Result<gpu::TextureView> targetView = adapterDevice.createTextureView(
      target, gpu::TextureViewDescriptor{"GeodeCheckerboardTargetView"});
  if (targetView.hasError()) {
    return std::nullopt;
  }
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder = adapterDevice.createCommandEncoder();
  if (encoder.hasError()) {
    return std::nullopt;
  }
  gpu::Result<gpu::RenderPassEncoder*> pass = encoder.result()->beginRenderPass(
      gpu::RenderPassDescriptor{"GeodeCheckerboardPass",
                                {gpu::RenderPassColorAttachment{
                                    targetView.result(), gpu::LoadOp::Load, gpu::StoreOp::Store}}});
  if (pass.hasError()) {
    return std::nullopt;
  }
  const GeodeCheckerboardPipeline& pipeline = PipelineForBlendMode(device, blendMode);
  if (!pipeline.valid() || pass.result()->setPipeline(pipeline.pipeline()).hasError()) {
    return std::nullopt;
  }
  device.countPipelineSwitch();
  return PreparedTargetPass{std::move(targetView).result(), std::move(encoder).result(),
                            pass.result(), &pipeline};
}

}  // namespace

bool GeodeCheckerboardPass::ensureResources(GeodeDevice& device,
                                            const GeodeCheckerboardPipeline& pipeline,
                                            GeodeCheckerboardPipeline::BlendMode blendMode) {
  if (bindGroup_.isValid() && bindGroupBlendMode_ == blendMode) {
    return true;
  }
  bindGroup_ = gpu::BindGroup();

  GeodeWgpuAdapterDevice& adapterDevice = device.adapterDevice();
  if (!uniformBuffer_.isValid()) {
    gpu::Result<gpu::Buffer> uniformBuffer = adapterDevice.createBuffer(gpu::BufferDescriptor{
        "GeodeCheckerboardUniforms", sizeof(GeodeCheckerboardPipeline::Uniforms),
        gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst});
    if (uniformBuffer.hasError()) {
      return false;
    }
    uniformBuffer_ = std::move(uniformBuffer).result();
  }

  gpu::Result<gpu::BindGroup> bindGroup = adapterDevice.createBindGroup(gpu::BindGroupDescriptor{
      "GeodeCheckerboardBG",
      pipeline.bindGroupLayout(),
      {gpu::BindGroupEntry{
          pipeline.uniformBinding(),
          gpu::BufferBinding{uniformBuffer_, 0, sizeof(GeodeCheckerboardPipeline::Uniforms)}}}});
  if (bindGroup.hasError()) {
    return false;
  }
  bindGroup_ = std::move(bindGroup).result();

  bindGroupBlendMode_ = blendMode;
  return true;
}

bool GeodeCheckerboardPass::draw(GeodeDevice& device, const gpu::Texture& target,
                                 Vector2i targetSizePx, const CheckerboardUnderlayParams& params,
                                 GeodeCheckerboardPipeline::BlendMode blendMode) {
  if (!CheckerboardRequestIsDrawable(target, targetSizePx, params)) {
    return false;
  }

  const gpu::Result<gpu::Extent2d> targetExtent = device.adapterDevice().textureExtent(target);
  if (targetExtent.hasError() ||
      targetExtent.result() != gpu::Extent2d{static_cast<std::uint32_t>(targetSizePx.x),
                                             static_cast<std::uint32_t>(targetSizePx.y)}) {
    return false;
  }
  std::optional<PreparedTargetPass> prepared = PrepareTargetPass(device, target, blendMode);
  if (!prepared.has_value() || !ensureResources(device, *prepared->pipeline, blendMode)) {
    return false;
  }
  const GeodeCheckerboardPipeline::Uniforms uniforms{
      .targetSize = {static_cast<float>(targetSizePx.x), static_cast<float>(targetSizePx.y)},
      .devicePixelRatio = static_cast<float>(params.devicePixelRatio),
      .checkerSize = static_cast<float>(params.cellSizeLogicalPx),
      .darkColor = {params.darkColor[0], params.darkColor[1], params.darkColor[2],
                    params.darkColor[3]},
      .lightColor = {params.lightColor[0], params.lightColor[1], params.lightColor[2],
                     params.lightColor[3]},
      .originOffsetPx = {static_cast<float>(params.originOffsetPx.x),
                         static_cast<float>(params.originOffsetPx.y)},
      .padding = {0.0f, 0.0f},
  };
  GeodeWgpuAdapterDevice& adapterDevice = device.adapterDevice();
  if (adapterDevice
          .writeBuffer(uniformBuffer_, 0,
                       std::span(reinterpret_cast<const uint8_t*>(&uniforms), sizeof(uniforms)))
          .hasError()) {
    return false;
  }

  if (params.scissorPx.has_value()) {
    (void)prepared->renderPass->setScissorRect(params.scissorPx->x, params.scissorPx->y,
                                               params.scissorPx->width, params.scissorPx->height);
  }
  (void)prepared->renderPass->setBindGroup(0, bindGroup_);
  (void)prepared->renderPass->draw(3, 1, 0, 0);
  (void)prepared->renderPass->end();
  gpu::Result<gpu::CommandBuffer> commandBuffer = prepared->encoder->finish();
  if (commandBuffer.hasError()) {
    return false;
  }
  return !adapterDevice.submit(std::move(commandBuffer).result()).hasError();
}

}  // namespace donner::geode
