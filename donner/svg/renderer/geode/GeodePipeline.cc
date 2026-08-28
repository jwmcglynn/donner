#include "donner/svg/renderer/geode/GeodePipeline.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
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

const gpu::RenderPipeline& GeodePipeline::batchedPipeline() const {
  if (!batchedPipeline_.isValid()) {
    // Identical state to the default pipeline - same layout, same shader
    // module, same blending - differing only in the entry points that read
    // paint and geometry from each instance's record. Built on first use
    // because only a cross-entity batch needs it.
    batchedPipeline_ = buildPipeline("GeodeSlugFillBatched", "vs_main_batched", "fs_main_batched");
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
}

// ============================================================================
// GeodeSnapshotReadbackPipeline
// ============================================================================

GeodeSnapshotReadbackPipeline::GeodeSnapshotReadbackPipeline(gpu::Device& device) {
  gpu::shader::ShaderResult<gpu::shader::IrModule> module =
      gpu::shader::programs::BuildSnapshotUnpremultiplyModule();
  if (module.hasError()) {
    return;
  }
  gpu::shader::ShaderResult<std::string> wgsl = gpu::shader::EmitWgsl(module.result());
  if (wgsl.hasError()) {
    return;
  }

  // The workgroup size travels with the module rather than being restated here: the runtime
  // checks the pipeline's declared size against the entry point's, and a second literal is
  // exactly the disagreement that check exists to catch.
  gpu::Result<gpu::ShaderModule> shaderModule = device.createShaderModule(
      gpu::ShaderModuleDescriptor{"GeodeSnapshotReadbackShader",
                                  RcString(wgsl.result()),
                                  gpu::ShaderSourceKind::Wgsl,
                                  {},
                                  gpu::shader::ComputeEntryPointsOf(module.result())});
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

  constexpr uint32_t kWorkgroup = gpu::shader::programs::kSnapshotUnpremultiplyWorkgroupSize;
  gpu::Result<gpu::ComputePipeline> pipeline =
      device.createComputePipeline(gpu::ComputePipelineDescriptor{
          "GeodeSnapshotReadback", pipelineLayout_, gpu::ComputeState{shaderModule_, "cs_main"},
          gpu::WorkgroupSize{kWorkgroup, kWorkgroup, 1}});
  if (pipeline.hasError()) {
    return;
  }
  pipeline_ = std::move(pipeline).result();
}

}  // namespace donner::geode
