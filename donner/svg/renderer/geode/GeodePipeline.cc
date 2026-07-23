#include "donner/svg/renderer/geode/GeodePipeline.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
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
  // ----- Bind group layout -----
  // Twelve bindings: uniforms, H bands SSBO, H curves SSBO, pattern
  // texture, pattern sampler, clip-mask texture, clip-mask sampler, the
  // per-instance records SSBO (transform, color, rule, grid parameters,
  // bounding polygon, geometry bases; 256 bytes per record), V bands
  // SSBO, V curves SSBO, the combined dense grid storage, and the gradient
  // paint blocks. The pattern texture/sampler are only sampled when
  // paintMode == "pattern" and the clip-mask texture/sampler only when
  // `hasClipMask != 0`; a 1x1 dummy texture is bound for both when the
  // feature is inactive so the bind group layout is stable across draw
  // calls. Every draw binds at least one record; instanced and batched
  // draws bind a contiguous record span indexed by instance_index.
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
      // Per-instance records: the vertex stage reads the transform + bounding polygon, and the
      // fragment stage reads color / rule / grid / geometry bases through a flat instance-id
      // varying, so overlapping batched instances still blend in painter order.
      gpu::BindGroupLayoutEntry{7, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                gpu::BindingType::ReadOnlyStorageBuffer},
      FragmentStorageEntry(8),
      FragmentStorageEntry(9),
      // The four dense grid arrays (hBandGrid, vBandGrid, hCurveIndices, vCurveIndices) share ONE
      // combined u32 storage binding; instance records carry the element bases. This keeps the
      // fragment stage at seven storage bindings, under the baseline WebGPU limit of eight per
      // stage.
      FragmentStorageEntry(10),
      // Gradient paint blocks, addressed by each record's element base, so a run of differently
      // painted gradient fills shares one draw instead of rebinding a per-draw gradient uniform.
      FragmentStorageEntry(11),
  };
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
  pipeline_ = buildPipeline("GeodeSlugFill", "vs_main", "fs_main");

  borrowedPipeline_ = adapterDevice.wgpuRenderPipelineOf(pipeline_);
  borrowedBindGroupLayout_ = adapterDevice.wgpuBindGroupLayoutOf(bindGroupLayout_);
}

gpu::RenderPipeline GeodePipeline::buildPipeline(const char* label, const char* vertexEntryPoint,
                                                 const char* fragmentEntryPoint) const {
  return UnwrapOrAbort(
      adapterDevice_->createRenderPipeline(gpu::RenderPipelineDescriptor{
          label, pipelineLayout_, gpu::VertexState{shaderModule_, vertexEntryPoint, {}},
          gpu::FragmentState{shaderModule_,
                             fragmentEntryPoint,
                             {gpu::ColorTargetState{colorFormat_, PremultipliedSourceOverBlend()}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      label);
}

const wgpu::RenderPipeline& GeodePipeline::batchedPipeline() const {
  if (!batchedPipeline_.isValid()) {
    // Identical state to the default pipeline - same layout, same shader
    // module, same blending - differing only in the entry points that read
    // paint and geometry from each instance's record. Built on first use
    // because only a cross-entity batch needs it.
    batchedPipeline_ = buildPipeline("GeodeSlugFillBatched", "vs_main_batched", "fs_main_batched");
    borrowedBatchedPipeline_ = adapterDevice_->wgpuRenderPipelineOf(batchedPipeline_);
  }
  return borrowedBatchedPipeline_;
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

  borrowedPipeline_ = adapterDevice.wgpuRenderPipelineOf(pipeline_);
  borrowedBindGroupLayout_ = adapterDevice.wgpuBindGroupLayoutOf(bindGroupLayout_);
}

// ============================================================================
// GeodeMaskPipeline
// ============================================================================

GeodeMaskPipeline::GeodeMaskPipeline(GeodeWgpuAdapterDevice& adapterDevice) {
  // Eleven bindings - uniforms, H bands SSBO, H curves SSBO, nested clip mask
  // texture, nested clip mask sampler, and (analytic dual-ray) V bands SSBO,
  // V curves SSBO, H band grid, V band grid, and compact references into each
  // canonical curve array. The clip-mask slot is always bound; a 1x1 dummy is
  // used when `uniforms.hasClipMask == 0`.
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
          "GeodeSlugMask", pipelineLayout_, gpu::VertexState{shaderModule_, "vs_main", {}},
          gpu::FragmentState{shaderModule_,
                             "fs_main",
                             {gpu::ColorTargetState{gpu::TextureFormat::RGBA8Unorm, maxBlend}}},
          gpu::PrimitiveTopology::TriangleList, gpu::CullMode::None}),
      "GeodeSlugMask createRenderPipeline");

  borrowedPipeline_ = adapterDevice.wgpuRenderPipelineOf(pipeline_);
  borrowedBindGroupLayout_ = adapterDevice.wgpuBindGroupLayoutOf(bindGroupLayout_);
}

// ============================================================================
// GeodeSnapshotReadbackPipeline
// ============================================================================

GeodeSnapshotReadbackPipeline::GeodeSnapshotReadbackPipeline(const wgpu::Device& device) {
  // Two bindings: the premultiplied render target (sampled via textureLoad)
  // and the straight-alpha RGBA8 staging storage texture.
  wgpu::BindGroupLayoutEntry entries[2] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[0].texture.multisampled = false;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[1].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSnapshotReadbackBGL");
  bglDesc.entryCount = 2;
  bglDesc.entries = entries;
  bindGroupLayout_.reset(device.createBindGroupLayout(bglDesc));

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSnapshotReadbackPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_.get()};
  plDesc.bindGroupLayouts = layouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(device.createPipelineLayout(plDesc));

  ScopedWgpuHandle<wgpu::ShaderModule> shader(createSnapshotUnpremultiplyShader(device));

  wgpu::ComputePipelineDescriptor cpDesc = {};
  cpDesc.label = wgpuLabel("GeodeSnapshotReadback");
  cpDesc.layout = pipelineLayout.get();
  cpDesc.compute.module = shader.get();
  cpDesc.compute.entryPoint = wgpuLabel("main");
  pipeline_.reset(device.createComputePipeline(cpDesc));
}

}  // namespace donner::geode
