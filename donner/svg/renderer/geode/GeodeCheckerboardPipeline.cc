#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

namespace {

constexpr std::string_view kCheckerboardWgsl = R"wgsl(
struct Params {
  target_size: vec2<f32>,
  device_pixel_ratio: f32,
  checker_size: f32,
  dark_color: vec4<f32>,
  light_color: vec4<f32>,
  origin_offset: vec2<f32>,
  padding: vec2<f32>,
};

@group(0) @binding(0) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>(3.0, -1.0),
    vec2<f32>(-1.0, 3.0)
  );
  return vec4<f32>(positions[vertex_index], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  // `origin_offset` shifts the pattern's anchor away from the target's top-left
  // so a target that is itself placed somewhere on screen can pin the same
  // cells a window-anchored pattern would have drawn there.
  let anchored = min(position.xy, params.target_size) + params.origin_offset;
  let screen = anchored / max(params.device_pixel_ratio, 0.0001);
  let cell = vec2<i32>(floor(screen / vec2<f32>(params.checker_size, params.checker_size)));
  // Bitwise parity, not `% 2`: a negative offset produces negative cell indices
  // and WGSL's `%` keeps the sign, so `(-1) % 2 == -1` would break the
  // alternation at the anchor boundary.
  if (((cell.x + cell.y) & 1) == 0) {
    return params.light_color;
  }

  return params.dark_color;
}
)wgsl";

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
bool CheckerboardRequestIsDrawable(const wgpu::Texture& target, Vector2i targetSizePx,
                                   const CheckerboardUnderlayParams& params) {
  if (!target || targetSizePx.x <= 0 || targetSizePx.y <= 0) {
    return false;
  }
  if (!(params.devicePixelRatio > 0.0) || !(params.cellSizeLogicalPx > 0.0)) {
    return false;
  }
  return !params.scissorPx.has_value() ||
         (params.scissorPx->width != 0 && params.scissorPx->height != 0);
}

/// True when the device's command stream is free for a submission of our own.
///
/// While a renderer has a frame open it owns that stream: the runtime device replays every
/// submission into the frame's command buffer so the whole frame keeps one recording order.
/// A checkerboard submitted there would be spliced into a command buffer it does not own, at a
/// point in that buffer it cannot reason about, and would not reach the target when its caller
/// expects it to. There is also no ordering it could pick instead: it cannot know whether the
/// open frame draws to the same target, so queueing itself ahead of that frame would be right
/// for an overwriting checkerboard and wrong for one that goes underneath. Declining is the
/// only answer that stays true to `draw`'s contract, and no caller needs the overlap - the
/// presentation path draws the checkerboard before it opens its frame.
bool DeviceCommandStreamIsFree(GeodeDevice& device) {
  if (!device.adapterDevice().hasHostCommandEncoder()) {
    return true;
  }
  std::fprintf(stderr,
               "[Geode] checkerboard skipped: another frame owns the device's command stream. "
               "Draw it outside that frame.\n");
  return false;
}

/// A surface-owned render target named for the runtime, plus the view a pass attaches.
struct NamedTarget {
  gpu::Texture texture;   //!< Runtime name for the borrowed target; owns no backing.
  gpu::TextureView view;  //!< Whole-texture view used as the pass attachment.
};

/// Names @p target for the runtime so a pass can attach it. The target belongs to the embedding
/// surface, so its capabilities are read from the texture itself and cannot be described wrongly.
/// Returns nothing when the runtime refuses either handle.
std::optional<NamedTarget> NameTargetForPass(GeodeWgpuAdapterDevice& adapterDevice,
                                             const wgpu::Texture& target) {
  gpu::Result<gpu::Texture> texture = adapterDevice.importExternalTexture(
      target, gpu::Extent2d{target.getWidth(), target.getHeight()},
      GpuTextureFormatFromWgpu(target.getFormat()), GpuTextureUsageFromWgpu(target.getUsage()));
  if (texture.hasError()) {
    return std::nullopt;
  }
  gpu::Result<gpu::TextureView> view = adapterDevice.createTextureView(
      texture.result(), gpu::TextureViewDescriptor{"GeodeCheckerboardTargetView"});
  if (view.hasError()) {
    return std::nullopt;
  }
  return NamedTarget{std::move(texture).result(), std::move(view).result()};
}

/// Records the fullscreen-triangle pass into its own command buffer and submits it. The pass
/// encoder latches its first error and reports it from `finish`, so the individual pass
/// operations are checked once there rather than one at a time.
bool RecordAndSubmitCheckerboardPass(GeodeDevice& device,
                                     const GeodeCheckerboardPipeline& checkerboard,
                                     const gpu::TextureView& targetView,
                                     const gpu::BindGroup& bindGroup,
                                     const std::optional<CheckerboardScissorPx>& scissorPx) {
  GeodeWgpuAdapterDevice& adapterDevice = device.adapterDevice();
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder = adapterDevice.createCommandEncoder();
  if (encoder.hasError()) {
    return false;
  }
  gpu::CommandEncoder& commands = *encoder.result();

  gpu::Result<gpu::RenderPassEncoder*> pass = commands.beginRenderPass(gpu::RenderPassDescriptor{
      "GeodeCheckerboardPass",
      {gpu::RenderPassColorAttachment{targetView, gpu::LoadOp::Load, gpu::StoreOp::Store}}});
  if (pass.hasError()) {
    return false;
  }

  gpu::RenderPassEncoder& renderPass = *pass.result();
  if (scissorPx.has_value()) {
    (void)renderPass.setScissorRect(scissorPx->x, scissorPx->y, scissorPx->width,
                                    scissorPx->height);
  }
  (void)renderPass.setPipeline(checkerboard.pipeline());
  device.countPipelineSwitch();
  (void)renderPass.setBindGroup(0, bindGroup);
  (void)renderPass.draw(3, 1, 0, 0);
  (void)renderPass.end();

  gpu::Result<gpu::CommandBuffer> commandBuffer = commands.finish();
  if (commandBuffer.hasError()) {
    return false;
  }
  return !adapterDevice.submit(std::move(commandBuffer).result()).hasError();
}

}  // namespace

GeodeCheckerboardPipeline::GeodeCheckerboardPipeline(GeodeWgpuAdapterDevice& adapterDevice,
                                                     gpu::TextureFormat colorFormat,
                                                     BlendMode blendMode) {
  gpu::Result<gpu::BindGroupLayout> bindGroupLayout =
      adapterDevice.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "GeodeCheckerboardBGL",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                     gpu::BindingType::UniformBuffer}}});
  if (bindGroupLayout.hasError()) {
    return;
  }
  bindGroupLayout_ = std::move(bindGroupLayout).result();

  gpu::Result<gpu::PipelineLayout> pipelineLayout = adapterDevice.createPipelineLayout(
      gpu::PipelineLayoutDescriptor{"GeodeCheckerboardPL", {bindGroupLayout_}});
  if (pipelineLayout.hasError()) {
    return;
  }
  pipelineLayout_ = std::move(pipelineLayout).result();

  gpu::Result<gpu::ShaderModule> shaderModule =
      adapterDevice.createShaderModule(gpu::ShaderModuleDescriptor{
          "GeodeCheckerboard", RcString(kCheckerboardWgsl), gpu::ShaderSourceKind::Wgsl});
  if (shaderModule.hasError()) {
    return;
  }
  shaderModule_ = std::move(shaderModule).result();

  // `result = dst + src * (1 - dst.alpha)` on both color and alpha. The shader
  // emits an opaque checker color, so fully-transparent destination pixels take
  // the checker outright, fully-opaque ones are untouched, and partially
  // transparent premultiplied content blends over it exactly as `destination-
  // over` does in Canvas2D / Skia.
  const gpu::BlendComponent destinationOver{gpu::BlendFactor::OneMinusDstAlpha,
                                            gpu::BlendFactor::One, gpu::BlendOperation::Add};

  gpu::ColorTargetState colorTarget{colorFormat};
  if (blendMode == BlendMode::DestinationOver) {
    colorTarget.blend = gpu::BlendState{destinationOver, destinationOver};
  }

  gpu::Result<gpu::RenderPipeline> pipeline =
      adapterDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeCheckerboard", pipelineLayout_, gpu::VertexState{shaderModule_, "vs_main", {}},
          gpu::FragmentState{shaderModule_, "fs_main", {colorTarget}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None});
  if (pipeline.hasError()) {
    return;
  }
  pipeline_ = std::move(pipeline).result();
}

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
          0, gpu::BufferBinding{uniformBuffer_, 0, sizeof(GeodeCheckerboardPipeline::Uniforms)}}}});
  if (bindGroup.hasError()) {
    return false;
  }
  bindGroup_ = std::move(bindGroup).result();

  bindGroupBlendMode_ = blendMode;
  return true;
}

bool GeodeCheckerboardPass::draw(GeodeDevice& device, const wgpu::Texture& target,
                                 Vector2i targetSizePx, const CheckerboardUnderlayParams& params,
                                 GeodeCheckerboardPipeline::BlendMode blendMode) {
  if (!CheckerboardRequestIsDrawable(target, targetSizePx, params) ||
      !DeviceCommandStreamIsFree(device)) {
    return false;
  }

  const GeodeCheckerboardPipeline& checkerboard = PipelineForBlendMode(device, blendMode);
  if (!checkerboard.valid() || !ensureResources(device, checkerboard, blendMode)) {
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

  const std::optional<NamedTarget> named = NameTargetForPass(adapterDevice, target);
  if (!named.has_value()) {
    return false;
  }
  return RecordAndSubmitCheckerboardPass(device, checkerboard, named->view, bindGroup_,
                                         params.scissorPx);
}

}  // namespace donner::geode
