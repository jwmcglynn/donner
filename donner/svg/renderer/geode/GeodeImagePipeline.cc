#include "donner/svg/renderer/geode/GeodeImagePipeline.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

namespace {

/// Unwraps a `donner::gpu` creation result, halting on failure (see GeodePipeline.cc's
/// UnwrapOrAbort for the rationale).
template <typename T>
T UnwrapOrAbort(gpu::Result<T>&& result, const char* what) {
  if (result.hasError()) {
    std::fprintf(stderr, "[Geode] %s failed: %s\n", what, result.error().message.c_str());
    UTILS_RELEASE_ASSERT_MSG(false, "Geode image pipeline construction failed");
  }
  return std::move(result).result();
}

}  // namespace

GeodeImagePipeline::GeodeImagePipeline(GeodeWgpuAdapterDevice& adapterDevice,
                                       gpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  const auto& shader = gpu::shader::programs::ImageBlitShader();
  const auto entries = gpu::shader::MakeBindingLayout(shader);
  bindGroupLayout_ =
      UnwrapOrAbort(adapterDevice.createBindGroupLayout(
                        gpu::BindGroupLayoutDescriptor{"GeodeImageBlitBGL", entries}),
                    "GeodeImageBlitBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(adapterDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeImageBlitPL", {bindGroupLayout_}}),
                                  "GeodeImageBlitPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createImageBlitShader(adapterDevice), "ImageBlit shader module");

  // ----- Fragment / blending -----
  // Same premultiplied-source-over as the Slug fill pipeline. The fragment
  // shader premultiplies the straight-alpha texture sample before emitting
  // the color so this blend equation is correct.
  const gpu::BlendState blend{
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                          gpu::BlendOperation::Add},
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                          gpu::BlendOperation::Add}};

  // ----- Render pipeline -----
  // No vertex buffers - the shader generates corners from vertex_index.
  pipeline_ = UnwrapOrAbort(
      adapterDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeImageBlit", pipelineLayout_,
          gpu::VertexState{shaderModule_, RcString(shader.entryPoints[0].name.view()), {}},
          gpu::FragmentState{shaderModule_,
                             RcString(shader.entryPoints[1].name.view()),
                             {gpu::ColorTargetState{colorFormat_, blend}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeImageBlit createRenderPipeline");

  // ----- Samplers -----
  // Linear (bilinear) sampler - the default for SVG's "smooth" image
  // rendering. Clamp-to-edge addressing matches the previous wgpu defaults.
  linearSampler_ =
      UnwrapOrAbort(adapterDevice.createSampler(gpu::SamplerDescriptor{
                        "GeodeImageBlitLinear", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                        gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge}),
                    "GeodeImageBlitLinear createSampler");

  // Nearest sampler for crisp-edge and explicit nearest sampling.
  nearestSampler_ =
      UnwrapOrAbort(adapterDevice.createSampler(gpu::SamplerDescriptor{
                        "GeodeImageBlitNearest", gpu::FilterMode::Nearest, gpu::FilterMode::Nearest,
                        gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge}),
                    "GeodeImageBlitNearest createSampler");
}

}  // namespace donner::geode
