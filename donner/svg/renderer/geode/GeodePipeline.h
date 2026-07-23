#pragma once
/// @file
/// Render pipeline for the Slug fill algorithm.

#include "donner/gpu/Device.h"

namespace donner::geode {

/**
 * Caches a compiled render pipeline for the Slug fill shader, plus its bind group layout.
 *
 * One `GeodePipeline` instance is sufficient per `(device, render-target-format)`
 * pair - the actual data (uniforms, vertex buffers, bands, curves) varies per
 * draw call but the pipeline state object can be reused.
 *
 * The pipeline is created through the \c donner::gpu runtime (design 0053 packet 8): the class
 * holds the RAII `donner::gpu` handles, and GeoEncoder records against them through
 * `gpu::CommandEncoder` (packet 8b).
 *
 * The bind group layout matches the shader in `shaders/slug_fill.wgsl`:
 * - binding 0: uniform buffer (Uniforms struct: mvp, patternFromPath, viewport,
 *   tileSize, color, fillRule, paintMode, patternOpacity)
 * - binding 1: storage buffer (read-only) - Band[]
 * - binding 2: storage buffer (read-only) - curve data (flat f32[])
 * - binding 3: pattern tile texture (2D, Float sampleType) - sampled only
 *   when paintMode == 1. A 1x1 dummy texture is bound in solid-fill draws.
 * - binding 4: pattern sampler (Filtering) - paired with binding 3.
 * - bindings 5-6: clip-mask texture + sampler (Phase 3b).
 * - binding 7: per-instance transforms SSBO (M6 Bullet 2, vertex-visible).
 * - bindings 8-11: vertical bands/curves + band grids (analytic dual-ray, 0041 s8).
 *
 * The vertex buffer layout is:
 * - location 0: vec2f position (offset 0)
 * - location 1: vec2f normal   (offset 8)
 * - location 2: u32 bandIndex  (offset 16)
 * Stride: 20 bytes per vertex.
 */
class GeodePipeline {
public:
  /**
   * Create a Slug fill pipeline for the given device and color target format.
   *
   * @param gpuDevice The Donner GPU device (in the transition phase, the GeodeDevice-owned
   *   wgpu adapter) the pipeline objects are created on.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodePipeline(gpu::Device& gpuDevice, gpu::TextureFormat colorFormat);

  ~GeodePipeline() = default;
  GeodePipeline(const GeodePipeline&) = delete;
  GeodePipeline& operator=(const GeodePipeline&) = delete;
  /// Move constructor.
  GeodePipeline(GeodePipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodePipeline& operator=(GeodePipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }

  /// The bind group layout used by the pipeline. Bind groups for draws MUST be created against
  /// this exact handle: `gpu::RenderPassEncoder::draw` validates bind-group-layout identity
  /// against the pipeline layout.
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }

  /// Color format the pipeline was built for.
  gpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  gpu::TextureFormat colorFormat_ = gpu::TextureFormat::RGBA8Unorm;
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;
};

/**
 * Caches a compiled render pipeline for the Slug gradient-fill shader plus its bind-group
 * layout.
 *
 * Parallel to @ref GeodePipeline but with a larger uniform buffer that carries
 * linear-gradient parameters (pathFromGradient transform, start/end,
 * spread mode, per-stop colors and offsets). The vertex layout, Band / curve
 * storage bindings, and blend state are identical.
 *
 * Kept as a sibling class instead of a branch inside @ref GeodePipeline to
 * keep the solid-fill pipeline's 128-byte uniform layout untouched, and so
 * radial / sweep gradient pipelines can slot in alongside this one later
 * without churning the solid-fill path.
 */
class GeodeGradientPipeline {
public:
  /// Construct a gradient pipeline for the given device and color target format.
  GeodeGradientPipeline(gpu::Device& gpuDevice, gpu::TextureFormat colorFormat);

  ~GeodeGradientPipeline() = default;
  GeodeGradientPipeline(const GeodeGradientPipeline&) = delete;
  GeodeGradientPipeline& operator=(const GeodeGradientPipeline&) = delete;
  /// Move constructor.
  GeodeGradientPipeline(GeodeGradientPipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeGradientPipeline& operator=(GeodeGradientPipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }
  /// The bind group layout used by the pipeline (see GeodePipeline::bindGroupLayout on the
  /// layout-identity requirement).
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }
  /// Color format the pipeline was built for.
  gpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  gpu::TextureFormat colorFormat_ = gpu::TextureFormat::RGBA8Unorm;
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;
};

/**
 * Caches a compiled render pipeline for the path-clip mask shader
 * (`shaders/slug_mask.wgsl`) plus its bind-group layout.
 *
 * The mask pipeline is a stripped-down sibling of @ref GeodePipeline -
 * it reuses the same vertex shader and band/curve storage SSBOs. The fragment
 * stage replicates scalar analytic coverage into an `RGBA8Unorm` color
 * attachment. The resulting mask texture is then sampled by @ref GeodePipeline and
 * @ref GeodeGradientPipeline as a clip coverage multiplier.
 *
 * The bind group layout is:
 * - binding 0: uniform buffer (mvp, viewport, fillRule).
 * - binding 1: storage buffer (read-only) - Band[].
 * - binding 2: storage buffer (read-only) - curve data (flat f32[]).
 * - bindings 3-4: nested clip-mask texture + sampler.
 * - bindings 5-8: vertical bands/curves + band grids (analytic dual-ray, 0041 s8).
 *
 * Multiple paths belonging to a single clip layer are unioned on the hardware
 * side via `BlendOperation::Max`.
 */
class GeodeMaskPipeline {
public:
  /**
   * Create a Slug mask pipeline for the given device. Renders into a
   * single-sampled RGBA8Unorm texture.
   */
  explicit GeodeMaskPipeline(gpu::Device& gpuDevice);

  ~GeodeMaskPipeline() = default;
  GeodeMaskPipeline(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline& operator=(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline(GeodeMaskPipeline&&) noexcept = default;
  GeodeMaskPipeline& operator=(GeodeMaskPipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }
  /// The bind group layout used by the pipeline (see GeodePipeline::bindGroupLayout on the
  /// layout-identity requirement).
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }
  /// The color format the pipeline targets. Always `RGBA8Unorm`.
  gpu::TextureFormat colorFormat() const { return gpu::TextureFormat::RGBA8Unorm; }

private:
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;
};

}  // namespace donner::geode
