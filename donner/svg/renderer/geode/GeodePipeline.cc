#include "donner/svg/renderer/geode/GeodePipeline.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/shader/generated/SnapshotUnpremultiplyShader.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiplyBindings.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

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

GeodePipeline::GeodePipeline(GeodeWgpuAdapterDevice& adapterDevice, gpu::TextureFormat colorFormat)
    : adapterDevice_(&adapterDevice), colorFormat_(colorFormat) {
  const auto& shader = gpu::shader::programs::SlugFillShader();
  const auto entries = gpu::shader::MakeBindingLayout(shader);
  bindGroupLayout_ = UnwrapOrAbort(adapterDevice.createBindGroupLayout(
                                       gpu::BindGroupLayoutDescriptor{"GeodeSlugFillBGL", entries}),
                                   "GeodeSlugFillBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(adapterDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeSlugFillPL", {bindGroupLayout_}}),
                                  "GeodeSlugFillPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createSlugFillShader(adapterDevice), "SlugFill shader module");

  // The default entry points take the draw's paint and geometry from the uniform, which serves
  // every draw whose instances share one paint and one encoded path. `batchedPipeline` builds the
  // record-reading variant.
  pipeline_ = buildPipeline("GeodeSlugFill", shader.entryPoints[0].name.view(),
                            shader.entryPoints[2].name.view());
}

gpu::RenderPipeline GeodePipeline::buildPipeline(const char* label,
                                                 std::string_view vertexEntryPoint,
                                                 std::string_view fragmentEntryPoint) const {
  return UnwrapOrAbort(
      adapterDevice_->createRenderPipeline(gpu::RenderPipelineDescriptor{
          label, pipelineLayout_, gpu::VertexState{shaderModule_, RcString(vertexEntryPoint), {}},
          gpu::FragmentState{shaderModule_,
                             RcString(fragmentEntryPoint),
                             {gpu::ColorTargetState{colorFormat_, PremultipliedSourceOverBlend()}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      label);
}

const gpu::RenderPipeline& GeodePipeline::batchedPipeline() const {
  if (!batchedPipeline_.isValid()) {
    // Identical state to the default pipeline - same layout, same shader
    // module, same blending - differing only in the entry points that read
    // paint and geometry from each instance's record. Built on first use
    // because only a cross-entity batch needs it.
    const auto& shader = gpu::shader::programs::SlugFillShader();
    batchedPipeline_ = buildPipeline("GeodeSlugFillBatched", shader.entryPoints[1].name.view(),
                                     shader.entryPoints[3].name.view());
  }
  return batchedPipeline_;
}

// ============================================================================
// GeodeGradientPipeline
// ============================================================================

GeodeGradientPipeline::GeodeGradientPipeline(GeodeWgpuAdapterDevice& adapterDevice,
                                             gpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // Eleven bindings - uniforms, H bands SSBO, H curves SSBO, clip-mask texture,
  // clip-mask sampler, and (analytic dual-ray) V bands SSBO, V curves
  // SSBO, H band grid, V band grid, and compact references into each canonical
  // curve array. The clip-mask bindings always carry something valid; when
  // `hasClipMask == 0` a 1x1 dummy texture is bound and the shader skips the
  // sample work.
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
      FragmentStorageEntry(9),
      FragmentStorageEntry(10),
  };
  bindGroupLayout_ =
      UnwrapOrAbort(adapterDevice.createBindGroupLayout(
                        gpu::BindGroupLayoutDescriptor{"GeodeSlugGradientBGL", entries}),
                    "GeodeSlugGradientBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(adapterDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeSlugGradientPL", {bindGroupLayout_}}),
                                  "GeodeSlugGradientPL createPipelineLayout");

  shaderModule_ =
      UnwrapOrAbort(createSlugGradientShader(adapterDevice), "SlugGradient shader module");

  pipeline_ = UnwrapOrAbort(
      adapterDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeSlugGradient", pipelineLayout_, gpu::VertexState{shaderModule_, "vs_main", {}},
          gpu::FragmentState{shaderModule_,
                             "fs_main",
                             {gpu::ColorTargetState{colorFormat_, PremultipliedSourceOverBlend()}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeSlugGradient createRenderPipeline");
}

// ============================================================================
// GeodeMaskPipeline
// ============================================================================

GeodeMaskPipeline::GeodeMaskPipeline(GeodeWgpuAdapterDevice& adapterDevice) {
  const auto& shader = gpu::shader::programs::SlugMaskShader();
  const auto entries = gpu::shader::MakeBindingLayout(shader);
  bindGroupLayout_ = UnwrapOrAbort(adapterDevice.createBindGroupLayout(
                                       gpu::BindGroupLayoutDescriptor{"GeodeSlugMaskBGL", entries}),
                                   "GeodeSlugMaskBGL createBindGroupLayout");

  pipelineLayout_ = UnwrapOrAbort(adapterDevice.createPipelineLayout(gpu::PipelineLayoutDescriptor{
                                      "GeodeSlugMaskPL", {bindGroupLayout_}}),
                                  "GeodeSlugMaskPL createPipelineLayout");

  shaderModule_ = UnwrapOrAbort(createSlugMaskShader(adapterDevice), "SlugMask shader module");

  // Max-blend unions scalar analytic coverage from multiple clip paths.
  const gpu::BlendState maxBlend{
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::One, gpu::BlendOperation::Max},
      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::One, gpu::BlendOperation::Max}};

  pipeline_ = UnwrapOrAbort(
      adapterDevice.createRenderPipeline(gpu::RenderPipelineDescriptor{
          "GeodeSlugMask", pipelineLayout_,
          gpu::VertexState{shaderModule_, RcString(shader.entryPoints[0].name.view()), {}},
          gpu::FragmentState{shaderModule_,
                             RcString(shader.entryPoints[1].name.view()),
                             {gpu::ColorTargetState{gpu::TextureFormat::RGBA8Unorm, maxBlend}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeSlugMask createRenderPipeline");
}

// ============================================================================
// GeodeSnapshotReadbackPipeline
// ============================================================================

GeodeSnapshotReadbackPipeline::GeodeSnapshotReadbackPipeline(gpu::Device& device) {
  const gpu::ShaderModuleDescriptor descriptor =
      gpu::generated::snapshot_unpremultiply::BuildDescriptor(device.shaderSourceKind());
  gpu::Result<gpu::ShaderModule> shaderModule = device.createShaderModule(descriptor);
  if (shaderModule.hasError()) {
    return;
  }
  shaderModule_ = std::move(shaderModule).result();

  // Two bindings: the premultiplied render target read with textureLoad, and the straight-alpha
  // RGBA8 staging storage texture.
  using Binding = gpu::shader::programs::SnapshotUnpremultiplyBinding;
  gpu::Result<gpu::BindGroupLayout> bindGroupLayout =
      device.createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "GeodeSnapshotReadbackBGL",
          {gpu::BindGroupLayoutEntry{static_cast<uint32_t>(Binding::InputTexture),
                                     gpu::ShaderStage::Compute,
                                     gpu::BindingType::SampledTexture2dFloat},
           gpu::BindGroupLayoutEntry{
               static_cast<uint32_t>(Binding::OutputTexture), gpu::ShaderStage::Compute,
               gpu::BindingType::WriteOnlyStorageTexture2d, gpu::TextureFormat::RGBA8Unorm}}});
  if (bindGroupLayout.hasError()) {
    return;
  }
  bindGroupLayout_ = std::move(bindGroupLayout).result();

  gpu::Result<gpu::PipelineLayout> pipelineLayout = device.createPipelineLayout(
      gpu::PipelineLayoutDescriptor{"GeodeSnapshotReadbackPL", {bindGroupLayout_}});
  if (pipelineLayout.hasError()) {
    return;
  }
  pipelineLayout_ = std::move(pipelineLayout).result();

  gpu::Result<gpu::ComputePipeline> pipeline =
      device.createComputePipeline(gpu::ComputePipelineDescriptor{
          "GeodeSnapshotReadback", pipelineLayout_,
          gpu::ComputeState{shaderModule_, descriptor.computeEntryPoints.front().name},
          descriptor.computeEntryPoints.front().workgroupSize});
  if (pipeline.hasError()) {
    return;
  }
  pipeline_ = std::move(pipeline).result();
}

}  // namespace donner::geode
