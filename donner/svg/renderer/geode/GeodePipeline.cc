#include "donner/svg/renderer/geode/GeodePipeline.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"

namespace donner::geode {

namespace {

/// Unwraps a `donner::gpu` creation result, halting on failure: the pipeline family's
/// descriptors are compile-time-constant shapes against trusted build-embedded WGSL, so a
/// creation error here is a build defect (or a lost device), not a recoverable runtime state.
template <typename T>
T UnwrapOrAbort(gpu::Result<T>&& result, const char* what) {
  if (result.hasError()) {
    std::fprintf(stderr, "[Geode] %s failed: %s\n", what, result.error().message.c_str());
    UTILS_RELEASE_ASSERT_MSG(false, "Geode pipeline construction failed");
  }
  return std::move(result).result();
}

/// The Slug vertex buffer layout shared by the fill, gradient, and mask pipelines. Matches
/// EncodedPath::Vertex: pos (vec2f) + normal (vec2f) + bandIndex (u32) = 5 x 4 bytes = 20 bytes
/// per vertex.
gpu::VertexBufferLayout SlugVertexBufferLayout() {
  return gpu::VertexBufferLayout{20,
                                 gpu::VertexStepMode::Vertex,
                                 {gpu::VertexAttribute{gpu::VertexFormat::Float32x2, 0, 0},
                                  gpu::VertexAttribute{gpu::VertexFormat::Float32x2, 8, 1},
                                  gpu::VertexAttribute{gpu::VertexFormat::Uint32, 16, 2}}};
}

/// Standard premultiplied-alpha source-over blending used by the fill and gradient pipelines.
gpu::BlendState PremultipliedSourceOverBlend() {
  return gpu::BlendState{
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                          gpu::BlendOperation::Add},
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                          gpu::BlendOperation::Add}};
}

/// A fragment-visible read-only storage buffer entry at \p binding.
gpu::BindGroupLayoutEntry FragmentStorageEntry(uint32_t binding) {
  return gpu::BindGroupLayoutEntry{binding, gpu::ShaderStage::Fragment,
                                   gpu::BindingType::ReadOnlyStorageBuffer};
}

}  // namespace

GeodePipeline::GeodePipeline(gpu::Device& gpuDevice, gpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Twelve bindings: uniforms, bands SSBO, curves SSBO, pattern texture,
  // pattern sampler, clip-mask texture, clip-mask sampler, and
  // (Milestone 6 Bullet 2) per-instance transforms SSBO. The pattern
  // texture/sampler are only sampled when paintMode == "pattern" and
  // the clip-mask texture/sampler only when `hasClipMask != 0`. A 1x1
  // dummy texture is bound for both when the feature is inactive so
  // the bind group layout is stable across draw calls. The
  // instance-transforms buffer is always bound too - a 1-element
  // identity buffer (`GeodeDevice::identityInstanceTransformBuffer`)
  // for single-draw fills, a full per-instance array for
  // `fillPathInstanced`. Bindings 8-11 are the analytic dual-ray
  // (0041 s8) vertical bands/curves and band grids.
  const std::vector<gpu::BindGroupLayoutEntry> entries = {
      gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                gpu::BindingType::UniformBuffer},
      FragmentStorageEntry(1),
      FragmentStorageEntry(2),
      gpu::BindGroupLayoutEntry{3, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{4, gpu::ShaderStage::Fragment, gpu::BindingType::FilteringSampler},
      gpu::BindGroupLayoutEntry{5, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{6, gpu::ShaderStage::Fragment, gpu::BindingType::FilteringSampler},
      gpu::BindGroupLayoutEntry{7, gpu::ShaderStage::Vertex,
                                gpu::BindingType::ReadOnlyStorageBuffer},
      FragmentStorageEntry(8),
      FragmentStorageEntry(9),
      FragmentStorageEntry(10),
      FragmentStorageEntry(11),
  };
  bindGroupLayout_ = UnwrapOrAbort(
      gpuDevice.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{"GeodeSlugFillBGL", entries}),
      "GeodeSlugFillBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(gpuDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeSlugFillPL", {bindGroupLayout_}}),
                                  "GeodeSlugFillPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createSlugFillShader(gpuDevice), "SlugFill shader module");

  pipeline_ = UnwrapOrAbort(
      gpuDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeSlugFill", pipelineLayout_,
          gpu::VertexState{shaderModule_, "vs_main", {SlugVertexBufferLayout()}},
          gpu::FragmentState{shaderModule_,
                             "fs_main",
                             {gpu::ColorTargetState{colorFormat_, PremultipliedSourceOverBlend()}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeSlugFill createRenderPipeline");
}

// ============================================================================
// GeodeGradientPipeline
// ============================================================================

GeodeGradientPipeline::GeodeGradientPipeline(gpu::Device& gpuDevice, gpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // Nine bindings - uniforms, H bands SSBO, H curves SSBO, clip-mask texture,
  // clip-mask sampler, and (analytic dual-ray, 0041 s8) V bands SSBO, V curves
  // SSBO, H band grid, V band grid. The clip-mask bindings always carry
  // something valid; when `hasClipMask == 0` a 1x1 dummy texture is bound and
  // the shader skips the sample work.
  const std::vector<gpu::BindGroupLayoutEntry> entries = {
      gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                gpu::BindingType::UniformBuffer},
      FragmentStorageEntry(1),
      FragmentStorageEntry(2),
      gpu::BindGroupLayoutEntry{3, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{4, gpu::ShaderStage::Fragment, gpu::BindingType::FilteringSampler},
      FragmentStorageEntry(5),
      FragmentStorageEntry(6),
      FragmentStorageEntry(7),
      FragmentStorageEntry(8),
  };
  bindGroupLayout_ = UnwrapOrAbort(gpuDevice.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
                                       "GeodeSlugGradientBGL", entries}),
                                   "GeodeSlugGradientBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(gpuDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeSlugGradientPL", {bindGroupLayout_}}),
                                  "GeodeSlugGradientPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createSlugGradientShader(gpuDevice), "SlugGradient shader module");

  pipeline_ = UnwrapOrAbort(
      gpuDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeSlugGradient", pipelineLayout_,
          gpu::VertexState{shaderModule_, "vs_main", {SlugVertexBufferLayout()}},
          gpu::FragmentState{shaderModule_,
                             "fs_main",
                             {gpu::ColorTargetState{colorFormat_, PremultipliedSourceOverBlend()}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeSlugGradient createRenderPipeline");
}

// ============================================================================
// GeodeMaskPipeline
// ============================================================================

GeodeMaskPipeline::GeodeMaskPipeline(gpu::Device& gpuDevice) {
  // Nine bindings - uniforms, H bands SSBO, H curves SSBO, nested clip mask
  // texture, nested clip mask sampler, and (analytic dual-ray, 0041 s8) V bands
  // SSBO, V curves SSBO, H band grid, V band grid. The clip-mask slot is always
  // bound; a 1x1 dummy is used when `uniforms.hasClipMask == 0`.
  const std::vector<gpu::BindGroupLayoutEntry> entries = {
      gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                gpu::BindingType::UniformBuffer},
      FragmentStorageEntry(1),
      FragmentStorageEntry(2),
      gpu::BindGroupLayoutEntry{3, gpu::ShaderStage::Fragment,
                                gpu::BindingType::SampledTexture2dFloat},
      gpu::BindGroupLayoutEntry{4, gpu::ShaderStage::Fragment, gpu::BindingType::FilteringSampler},
      FragmentStorageEntry(5),
      FragmentStorageEntry(6),
      FragmentStorageEntry(7),
      FragmentStorageEntry(8),
  };
  bindGroupLayout_ = UnwrapOrAbort(
      gpuDevice.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{"GeodeSlugMaskBGL", entries}),
      "GeodeSlugMaskBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(gpuDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeSlugMaskPL", {bindGroupLayout_}}),
                                  "GeodeSlugMaskPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createSlugMaskShader(gpuDevice), "SlugMask shader module");

  // Max-blend unions scalar analytic coverage from multiple clip paths.
  const gpu::BlendState maxBlend{
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::One, gpu::BlendOperation::Max},
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::One, gpu::BlendOperation::Max}};

  pipeline_ = UnwrapOrAbort(
      gpuDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeSlugMask", pipelineLayout_,
          gpu::VertexState{shaderModule_, "vs_main", {SlugVertexBufferLayout()}},
          gpu::FragmentState{shaderModule_,
                             "fs_main",
                             {gpu::ColorTargetState{gpu::TextureFormat::RGBA8Unorm, maxBlend}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeSlugMask createRenderPipeline");
}

}  // namespace donner::geode
