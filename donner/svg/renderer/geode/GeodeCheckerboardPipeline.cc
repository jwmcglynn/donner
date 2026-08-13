#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"

#include <string_view>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

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

}  // namespace

GeodeCheckerboardPipeline::GeodeCheckerboardPipeline(const wgpu::Device& device,
                                                     wgpu::TextureFormat colorFormat,
                                                     BlendMode blendMode) {
  wgpu::BindGroupLayoutEntry layoutEntry = {};
  layoutEntry.binding = 0;
  layoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  layoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;
  layoutEntry.buffer.minBindingSize = sizeof(Uniforms);

  wgpu::BindGroupLayoutDescriptor layoutDesc = {};
  layoutDesc.label = wgpuLabel("GeodeCheckerboardBGL");
  layoutDesc.entryCount = 1;
  layoutDesc.entries = &layoutEntry;
  bindGroupLayout_ = device.createBindGroupLayout(layoutDesc);
  if (!bindGroupLayout_) {
    return;
  }

  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = kCheckerboardWgsl.data();
  wgslSource.code.length = kCheckerboardWgsl.size();

  wgpu::ShaderModuleDescriptor shaderDesc{wgpu::Default};
  shaderDesc.label = wgpuLabel("GeodeCheckerboard");
  shaderDesc.nextInChain = &wgslSource.chain;
  ScopedWgpuHandle<wgpu::ShaderModule> shader(device.createShaderModule(shaderDesc));
  if (!shader) {
    return;
  }

  wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {};
  pipelineLayoutDesc.label = wgpuLabel("GeodeCheckerboardPL");
  pipelineLayoutDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout bindGroupLayouts[1] = {bindGroupLayout_};
  pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(
      device.createPipelineLayout(pipelineLayoutDesc));
  if (!pipelineLayout) {
    return;
  }

  // `result = dst + src * (1 - dst.alpha)` on both color and alpha. The shader
  // emits an opaque checker color, so fully-transparent destination pixels take
  // the checker outright, fully-opaque ones are untouched, and partially
  // transparent premultiplied content blends over it exactly as `destination-
  // over` does in Canvas2D / Skia.
  wgpu::BlendState destinationOver = {};
  destinationOver.color.operation = wgpu::BlendOperation::Add;
  destinationOver.color.srcFactor = wgpu::BlendFactor::OneMinusDstAlpha;
  destinationOver.color.dstFactor = wgpu::BlendFactor::One;
  destinationOver.alpha.operation = wgpu::BlendOperation::Add;
  destinationOver.alpha.srcFactor = wgpu::BlendFactor::OneMinusDstAlpha;
  destinationOver.alpha.dstFactor = wgpu::BlendFactor::One;

  wgpu::ColorTargetState colorTarget = {};
  colorTarget.format = colorFormat;
  colorTarget.writeMask = wgpu::ColorWriteMask::All;
  if (blendMode == BlendMode::DestinationOver) {
    colorTarget.blend = &destinationOver;
  }

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = shader.get();
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor pipelineDesc = {};
  pipelineDesc.label = wgpuLabel("GeodeCheckerboard");
  pipelineDesc.layout = pipelineLayout.get();
  pipelineDesc.vertex.module = shader.get();
  pipelineDesc.vertex.entryPoint = wgpuLabel("vs_main");
  pipelineDesc.vertex.bufferCount = 0;
  pipelineDesc.vertex.buffers = nullptr;
  pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
  pipelineDesc.fragment = &fragmentState;
  pipelineDesc.multisample.count = 1;
  pipelineDesc.multisample.mask = 0xFFFFFFFF;
  pipeline_ = device.createRenderPipeline(pipelineDesc);
}

bool GeodeCheckerboardPass::ensureResources(GeodeDevice& device,
                                            const GeodeCheckerboardPipeline& pipeline,
                                            GeodeCheckerboardPipeline::BlendMode blendMode) {
  if (bindGroup_ && bindGroupBlendMode_ == blendMode) {
    return true;
  }
  bindGroup_.reset();

  if (!uniformBuffer_) {
    wgpu::BufferDescriptor bufferDesc = {};
    bufferDesc.label = wgpuLabel("GeodeCheckerboardUniforms");
    bufferDesc.size = sizeof(GeodeCheckerboardPipeline::Uniforms);
    bufferDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    uniformBuffer_.reset(device.device().createBuffer(bufferDesc));
    device.countBuffer();
    if (!uniformBuffer_) {
      return false;
    }
  }

  wgpu::BindGroupEntry bindGroupEntry = {};
  bindGroupEntry.binding = 0;
  bindGroupEntry.buffer = uniformBuffer_.get();
  bindGroupEntry.offset = 0;
  bindGroupEntry.size = sizeof(GeodeCheckerboardPipeline::Uniforms);

  wgpu::BindGroupDescriptor bindGroupDesc = {};
  bindGroupDesc.label = wgpuLabel("GeodeCheckerboardBG");
  bindGroupDesc.layout = pipeline.bindGroupLayout();
  bindGroupDesc.entryCount = 1;
  bindGroupDesc.entries = &bindGroupEntry;
  bindGroup_.reset(device.device().createBindGroup(bindGroupDesc));
  device.countBindGroup();
  if (!bindGroup_) {
    return false;
  }

  bindGroupBlendMode_ = blendMode;
  return true;
}

bool GeodeCheckerboardPass::draw(GeodeDevice& device, const wgpu::Texture& target,
                                 Vector2i targetSizePx, const CheckerboardUnderlayParams& params,
                                 GeodeCheckerboardPipeline::BlendMode blendMode) {
  if (!target || targetSizePx.x <= 0 || targetSizePx.y <= 0) {
    return false;
  }
  if (!(params.devicePixelRatio > 0.0) || !(params.cellSizeLogicalPx > 0.0)) {
    return false;
  }
  if (params.scissorPx.has_value() &&
      (params.scissorPx->width == 0 || params.scissorPx->height == 0)) {
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
  device.queue().writeBuffer(uniformBuffer_.get(), 0, &uniforms, sizeof(uniforms));
  device.countBufferWrite(sizeof(uniforms));

  ScopedWgpuHandle<wgpu::TextureView> view(target.createView());
  if (!view) {
    return false;
  }

  wgpu::CommandEncoderDescriptor encoderDesc = {};
  encoderDesc.label = wgpuLabel("GeodeCheckerboardEncoder");
  ScopedWgpuHandle<wgpu::CommandEncoder> encoder(device.device().createCommandEncoder(encoderDesc));
  if (!encoder) {
    return false;
  }

  wgpu::RenderPassColorAttachment color = {};
  color.view = view.get();
  color.loadOp = wgpu::LoadOp::Load;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 0.0, 0.0, 0.0};
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

  wgpu::RenderPassDescriptor passDesc = {};
  passDesc.label = wgpuLabel("GeodeCheckerboardPass");
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &color;
  ScopedWgpuHandle<wgpu::RenderPassEncoder> pass(encoder.get().beginRenderPass(passDesc));
  if (!pass) {
    return false;
  }

  if (params.scissorPx.has_value()) {
    pass.get().setScissorRect(params.scissorPx->x, params.scissorPx->y, params.scissorPx->width,
                              params.scissorPx->height);
  }
  pass.get().setPipeline(checkerboard.pipeline());
  pass.get().setBindGroup(0, bindGroup_.get(), 0, nullptr);
  pass.get().draw(3, 1, 0, 0);
  device.countPipelineSwitch();
  device.countDraw();
  pass.get().end();
  pass.reset();

  ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
  if (!commands) {
    return false;
  }
  device.queue().submit(1, &commands.get());
  device.countSubmit();
  return true;
}

}  // namespace donner::geode
