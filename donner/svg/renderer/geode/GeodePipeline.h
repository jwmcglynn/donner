#pragma once
/// @file
/// Render pipeline for the Slug fill algorithm.

#include <webgpu/webgpu.hpp>

#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

/**
 * Caches a compiled `wgpu::RenderPipeline` for the Slug fill shader, plus its
 * bind group layout.
 *
 * One `GeodePipeline` instance is sufficient per `(device, render-target-format)`
 * pair - the actual data (uniforms, bands, curves) varies per
 * draw call but the pipeline state object can be reused.
 *
 * The bind group layout matches the shader in `shaders/slug_fill.wgsl`:
 * - binding 0: uniform buffer (Uniforms struct: mvp, patternFromPath, viewport,
 *   tileSize, color, fillRule, paintMode, patternOpacity)
 * - binding 1: storage buffer (read-only) - Band[]
 * - binding 2: storage buffer (read-only) - curve data (flat f32[])
 * - binding 3: pattern tile texture (2D, Float sampleType) - sampled only
 *   when paintMode == 1. A 1x1 dummy texture is bound in solid-fill draws.
 * - binding 4: pattern sampler (Filtering) - paired with binding 3.
 * - binding 5: nested clip-mask texture.
 * - binding 6: nested clip-mask sampler.
 * - binding 7: per-instance transforms.
 * - bindings 8 and 9: vertical Band[] and canonical curve data.
 * - bindings 10 and 11: horizontal and vertical dense band grids.
 * - bindings 12 and 13: horizontal and vertical curve-reference indexes.
 *
 * The pipeline has no vertex buffer. Its shader expands the uniform bounding polygon into a
 * triangle fan from `vertex_index` and applies the half-pixel AA halo in device space.
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
  const wgpu::RenderPipeline& pipeline() const { return pipeline_.get(); }

  /// The bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_.get(); }

  /// Color format the pipeline was built for.
  wgpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  wgpu::TextureFormat colorFormat_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout_;
  ScopedWgpuHandle<wgpu::RenderPipeline> pipeline_;
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
  const wgpu::RenderPipeline& pipeline() const { return pipeline_.get(); }
  /// The bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_.get(); }
  /// Color format the pipeline was built for.
  wgpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  wgpu::TextureFormat colorFormat_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout_;
  ScopedWgpuHandle<wgpu::RenderPipeline> pipeline_;
};

/**
 * Caches a compiled `wgpu::RenderPipeline` for the path-clip mask shader
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
 * - bindings 5 and 6: vertical Band[] and canonical curve data.
 * - bindings 7 and 8: horizontal and vertical dense band grids.
 * - bindings 9 and 10: horizontal and vertical curve-reference indexes.
 *
 * No texture / sampler bindings - the mask pipeline doesn't read from
 * any texture input. Multiple paths belonging to a single clip layer
 * are unioned on the hardware side via `BlendOperation::Max`.
 */
class GeodeMaskPipeline {
public:
  /**
   * Create a Slug mask pipeline for the given device. Renders into an
   * single-sampled RGBA8Unorm texture.
   */
  explicit GeodeMaskPipeline(const wgpu::Device& device);

  ~GeodeMaskPipeline() = default;
  GeodeMaskPipeline(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline& operator=(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline(GeodeMaskPipeline&&) noexcept = default;
  GeodeMaskPipeline& operator=(GeodeMaskPipeline&&) noexcept = default;

  const wgpu::RenderPipeline& pipeline() const { return pipeline_.get(); }
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_.get(); }
  /// The color format the pipeline targets. Always `RGBA8Unorm`.
  wgpu::TextureFormat colorFormat() const { return wgpu::TextureFormat::RGBA8Unorm; }

private:
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout_;
  ScopedWgpuHandle<wgpu::RenderPipeline> pipeline_;
};

}  // namespace donner::geode
