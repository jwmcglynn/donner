#include "donner/svg/renderer/geode/GeodeImagePipeline.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"

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

GeodeImagePipeline::GeodeImagePipeline(gpu::Device& gpuDevice, gpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Seven bindings: uniform buffer, sampler, sampled content texture,
  // sampled luminance-mask texture (Phase 3c), sampled parent-
  // snapshot texture (Phase 3d blend modes), sampled Phase 3b
  // clip-mask texture, clip-mask sampler. Optional textures always
  // bind some valid view so the layout stays stable across every draw.
  const std::vector<gpu::BindGroupLayoutEntry> entries = {
      gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                gpu::BindingType::UniformBuffer},
      gpu::BindGroupLayoutEntry{1, gpu::ShaderStage::Fragment, gpu::BindingType::FilteringSampler},
      gpu::BindGroupLayoutEntry{2, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{3, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{4, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{5, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{6, gpu::ShaderStage::Fragment, gpu::BindingType::FilteringSampler},
  };
  bindGroupLayout_ = UnwrapOrAbort(
      gpuDevice.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{"GeodeImageBlitBGL", entries}),
      "GeodeImageBlitBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(gpuDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeImageBlitPL", {bindGroupLayout_}}),
                                  "GeodeImageBlitPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createImageBlitShader(gpuDevice), "ImageBlit shader module");

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
      gpuDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeImageBlit", pipelineLayout_, gpu::VertexState{shaderModule_, "vs_main", {}},
          gpu::FragmentState{
              shaderModule_, "fs_main", {gpu::ColorTargetState{colorFormat_, blend}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeImageBlit createRenderPipeline");

  // ----- Samplers -----
  // Linear (bilinear) sampler - the default for SVG's "smooth" image
  // rendering. Clamp-to-edge addressing matches the previous wgpu defaults.
  linearSampler_ =
      UnwrapOrAbort(gpuDevice.createSampler(gpu::SamplerDescriptor{
                        "GeodeImageBlitLinear", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                        gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge}),
                    "GeodeImageBlitLinear createSampler");

  // Nearest sampler - for `image-rendering: pixelated`.
  nearestSampler_ =
      UnwrapOrAbort(gpuDevice.createSampler(gpu::SamplerDescriptor{
                        "GeodeImageBlitNearest", gpu::FilterMode::Nearest, gpu::FilterMode::Nearest,
                        gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge}),
                    "GeodeImageBlitNearest createSampler");

  clipMaskSampler_ =
      UnwrapOrAbort(gpuDevice.createSampler(gpu::SamplerDescriptor{
                        "GeodeImageBlitClipMask", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                        gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge}),
                    "GeodeImageBlitClipMask createSampler");
}

}  // namespace donner::geode
