#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"

#include <utility>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/svg/renderer/geode/GeodeShaderSelection.h"

namespace donner::geode {

namespace {

/// The checkerboard projection \p device consumes. The adapter device takes WGSL; native Metal
/// and Vulkan devices exercise this same class through their own projection, which the
/// WebAssembly package never links.
/// @param device Device the pipeline is created on.
const gpu::shader::CompiledShaderView& SelectCheckerboardShader(const gpu::Device& device) {
  return SelectShaderProjection(device, DONNER_GEODE_SHADER_ARTIFACTS(Checkerboard));
}

/// True when \p shader exposes the vertex/fragment pair and its uniform block.
/// @param shader Static compiled interface.
bool HasCheckerboardInterface(const gpu::shader::CompiledShaderView& shader) {
  return shader.resource("params") != nullptr && shader.entryPoints.size() == 2 &&
         shader.entryPoints[0].stage == gpu::ShaderStage::Vertex &&
         shader.entryPoints[1].stage == gpu::ShaderStage::Fragment;
}

}  // namespace

GeodeCheckerboardPipeline::GeodeCheckerboardPipeline(gpu::Device& adapterDevice,
                                                     gpu::TextureFormat colorFormat,
                                                     BlendMode blendMode) {
  const gpu::shader::CompiledShaderView& shader = SelectCheckerboardShader(adapterDevice);
  if (!HasCheckerboardInterface(shader)) {
    return;
  }
  uniformBinding_ = shader.resource("params")->binding;
  gpu::Result<gpu::BindGroupLayout> bindGroupLayout =
      adapterDevice.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "GeodeCheckerboardBGL", gpu::shader::MakeBindingLayout(shader)});
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
      adapterDevice.createShaderModule(gpu::shader::MakeShaderDescriptor(
          shader, adapterDevice.shaderSourceKind(), "GeodeCheckerboard"));
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
          "GeodeCheckerboard", pipelineLayout_,
          gpu::VertexState{shaderModule_, RcString(shader.entryPoints[0].name.view()), {}},
          gpu::FragmentState{
              shaderModule_, RcString(shader.entryPoints[1].name.view()), {colorTarget}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None});
  if (pipeline.hasError()) {
    return;
  }
  pipeline_ = std::move(pipeline).result();
}

}  // namespace donner::geode
