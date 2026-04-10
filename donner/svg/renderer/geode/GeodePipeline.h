#pragma once
/// @file
/// Render pipeline for the Slug fill algorithm.

#include <webgpu/webgpu_cpp.h>

namespace donner::geode {

/**
 * Caches a compiled `wgpu::RenderPipeline` for the Slug fill shader, plus its
 * bind group layout.
 *
 * One `GeodePipeline` instance is sufficient per `(device, render-target-format)`
 * pair — the actual data (uniforms, vertex buffers, bands, curves) varies per
 * draw call but the pipeline state object can be reused.
 *
 * The bind group layout matches the shader in `shaders/slug_fill.wgsl`:
 * - binding 0: uniform buffer (Uniforms struct: mvp, patternFromPath, viewport,
 *   tileSize, color, fillRule, paintMode, patternOpacity)
 * - binding 1: storage buffer (read-only) — Band[]
 * - binding 2: storage buffer (read-only) — curve data (flat f32[])
 * - binding 3: pattern tile texture (2D, Float sampleType) — sampled only
 *   when paintMode == 1. A 1x1 dummy texture is bound in solid-fill draws.
 * - binding 4: pattern sampler (Filtering) — paired with binding 3.
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
   * @param device The WebGPU device.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat);

  ~GeodePipeline() = default;
  GeodePipeline(const GeodePipeline&) = delete;
  GeodePipeline& operator=(const GeodePipeline&) = delete;
  /// Move constructor.
  GeodePipeline(GeodePipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodePipeline& operator=(GeodePipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const wgpu::RenderPipeline& pipeline() const { return pipeline_; }

  /// The bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }

  /// Color format the pipeline was built for.
  wgpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  wgpu::TextureFormat colorFormat_;
  wgpu::BindGroupLayout bindGroupLayout_;
  wgpu::RenderPipeline pipeline_;
};

/**
 * Caches a compiled `wgpu::RenderPipeline` for the Slug gradient-fill shader
 * plus its bind-group layout.
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
  GeodeGradientPipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat);

  ~GeodeGradientPipeline() = default;
  GeodeGradientPipeline(const GeodeGradientPipeline&) = delete;
  GeodeGradientPipeline& operator=(const GeodeGradientPipeline&) = delete;
  /// Move constructor.
  GeodeGradientPipeline(GeodeGradientPipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeGradientPipeline& operator=(GeodeGradientPipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const wgpu::RenderPipeline& pipeline() const { return pipeline_; }
  /// The bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }
  /// Color format the pipeline was built for.
  wgpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  wgpu::TextureFormat colorFormat_;
  wgpu::BindGroupLayout bindGroupLayout_;
  wgpu::RenderPipeline pipeline_;
};

}  // namespace donner::geode
