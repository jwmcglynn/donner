#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"

#include <utility>

#include "donner/gpu/shader/generated/CheckerboardShader.h"

namespace donner::geode {

GeodeCheckerboardPipeline::GeodeCheckerboardPipeline(gpu::Device& adapterDevice,
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

  gpu::Result<gpu::ShaderModule> shaderModule = adapterDevice.createShaderModule(
      gpu::generated::checkerboard::BuildDescriptor(adapterDevice.shaderSourceKind()));
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

}  // namespace donner::geode
