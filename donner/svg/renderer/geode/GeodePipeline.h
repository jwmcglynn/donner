#pragma once
/// @file
/// Render pipeline for the Slug fill algorithm.

#include <webgpu/webgpu.hpp>

#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeWgpuAdapterDevice;

/**
 * Caches a compiled render pipeline for the Slug fill shader, plus its bind group layout.
 *
 * One `GeodePipeline` instance is sufficient per `(device, render-target-format)`
 * pair - the actual data (uniforms, bands, curves) varies per
 * draw call but the pipeline state object can be reused.
 *
 * The pipeline is created through the \c donner::gpu runtime (design 0053 packet 8): the class
 * holds the RAII `donner::gpu` handles, and - TEMPORARY for 8a while GeoEncoder still records
 * through wgpu - caches the borrowed wgpu objects resolved through the adapter's escape hatches
 * (deleted in packet 8b).
 *
 * The bind group layout matches the shader in `shaders/slug_fill.wgsl`:
 * - binding 0: uniform buffer (Uniforms struct: mvp, patternFromPath,
 *   viewport, tileSize, clip state, and the draw-level copy of the paint /
 *   geometry parameters)
 * - binding 1: storage buffer (read-only) - Band[]
 * - binding 2: storage buffer (read-only) - curve data (flat f32[])
 * - binding 3: pattern tile texture (2D, Float sampleType) - sampled only
 *   when paintMode == 1. A 1x1 dummy texture is bound in solid-fill draws.
 * - binding 4: pattern sampler (Filtering) - paired with binding 3.
 * - binding 5: nested clip-mask texture.
 * - binding 6: nested clip-mask sampler.
 * - binding 7: per-instance records. Only a cross-entity batch reads more
 *   than the transform here; every other draw binds the device's shared
 *   identity record.
 * - bindings 8 and 9: vertical Band[] and canonical curve data.
 * - binding 10: the four dense grid arrays in one combined storage range,
 *   indexed through the per-draw or per-instance element bases.
 * - binding 11: gradient paint blocks, addressed by each record's element base.
 *
 * The pipeline has no vertex buffer. Its shader expands the uniform bounding polygon into a
 * triangle fan from `vertex_index` and applies the half-pixel AA halo in device space.
 */
class GeodePipeline {
public:
  /**
   * Create a Slug fill pipeline for the given device and color target format.
   *
   * @param adapterDevice The Donner GPU device (wgpu adapter) owned by the GeodeDevice.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodePipeline(GeodeWgpuAdapterDevice& adapterDevice, gpu::TextureFormat colorFormat);

  ~GeodePipeline() = default;
  GeodePipeline(const GeodePipeline&) = delete;
  GeodePipeline& operator=(const GeodePipeline&) = delete;
  /// Move constructor.
  GeodePipeline(GeodePipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodePipeline& operator=(GeodePipeline&&) noexcept = default;

  /// The compiled render pipeline. Its entry points take the draw's paint
  /// and geometry parameters from the uniform, which serves every draw whose
  /// instances share one paint and one encoded path.
  /// (TEMPORARY wgpu alias for the still-wgpu GeoEncoder.)
  const wgpu::RenderPipeline& pipeline() const { return borrowedPipeline_; }

  /**
   * The cross-entity batch variant of @ref pipeline: same layout, same
   * shader module and same blending, but the entry points that take paint
   * and geometry from each instance's record. Compiled on first call,
   * because only a cross-entity batch needs it.
   * (TEMPORARY wgpu alias for the still-wgpu GeoEncoder.)
   */
  const wgpu::RenderPipeline& batchedPipeline() const;

  /// The bind group layout used by both pipelines.
  /// (TEMPORARY wgpu alias for the still-wgpu GeoEncoder.)
  const wgpu::BindGroupLayout& bindGroupLayout() const { return borrowedBindGroupLayout_; }

  /// Color format the pipeline was built for.
  gpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  /// Compile one variant of the Slug fill pipeline through the GPU runtime. Both variants share
  /// the layout, the shader module and the blend state; only the entry points differ.
  gpu::RenderPipeline buildPipeline(const char* label, const char* vertexEntryPoint,
                                    const char* fragmentEntryPoint) const;

  /// The device both pipeline variants are created through. Owned by the GeodeDevice that owns
  /// this pipeline, so it outlives every use here.
  GeodeWgpuAdapterDevice* adapterDevice_ = nullptr;
  gpu::TextureFormat colorFormat_ = gpu::TextureFormat::RGBA8Unorm;
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;
  /// Lazily compiled by @ref batchedPipeline. The `mutable` build is
  /// deliberately unsynchronized: a device's pipelines are only ever used
  /// from the single thread that owns rendering for that device, so two
  /// callers cannot reach the null check at once and the lazy build cannot
  /// race.
  mutable gpu::RenderPipeline batchedPipeline_;

  // TEMPORARY: borrowed wgpu aliases resolved through the adapter's escape hatches so the
  // still-wgpu GeoEncoder can bind them.
  wgpu::RenderPipeline borrowedPipeline_;
  wgpu::BindGroupLayout borrowedBindGroupLayout_;
  mutable wgpu::RenderPipeline borrowedBatchedPipeline_;
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
  GeodeGradientPipeline(GeodeWgpuAdapterDevice& adapterDevice, gpu::TextureFormat colorFormat);

  ~GeodeGradientPipeline() = default;
  GeodeGradientPipeline(const GeodeGradientPipeline&) = delete;
  GeodeGradientPipeline& operator=(const GeodeGradientPipeline&) = delete;
  /// Move constructor.
  GeodeGradientPipeline(GeodeGradientPipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeGradientPipeline& operator=(GeodeGradientPipeline&&) noexcept = default;

  /// The compiled render pipeline (TEMPORARY 8a wgpu alias for the still-wgpu GeoEncoder).
  const wgpu::RenderPipeline& pipeline() const { return borrowedPipeline_; }
  /// The bind group layout used by the pipeline (TEMPORARY 8a wgpu alias).
  const wgpu::BindGroupLayout& bindGroupLayout() const { return borrowedBindGroupLayout_; }
  /// Color format the pipeline was built for.
  gpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  gpu::TextureFormat colorFormat_ = gpu::TextureFormat::RGBA8Unorm;
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;

  // TEMPORARY 8a wgpu aliases; deleted in packet 8b (see GeodePipeline).
  wgpu::RenderPipeline borrowedPipeline_;
  wgpu::BindGroupLayout borrowedBindGroupLayout_;
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
 * - bindings 3 and 4: nested clip-mask texture + sampler.
 * - bindings 5 and 6: vertical Band[] and canonical curve data.
 * - bindings 7 and 8: horizontal and vertical dense band grids.
 * - bindings 9 and 10: horizontal and vertical curve-reference indexes.
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
  explicit GeodeMaskPipeline(GeodeWgpuAdapterDevice& adapterDevice);

  ~GeodeMaskPipeline() = default;
  GeodeMaskPipeline(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline& operator=(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline(GeodeMaskPipeline&&) noexcept = default;
  GeodeMaskPipeline& operator=(GeodeMaskPipeline&&) noexcept = default;

  /// The compiled render pipeline (TEMPORARY 8a wgpu alias for the still-wgpu GeoEncoder).
  const wgpu::RenderPipeline& pipeline() const { return borrowedPipeline_; }
  /// The bind group layout used by the pipeline (TEMPORARY 8a wgpu alias).
  const wgpu::BindGroupLayout& bindGroupLayout() const { return borrowedBindGroupLayout_; }
  /// The color format the pipeline targets. Always `RGBA8Unorm`.
  gpu::TextureFormat colorFormat() const { return gpu::TextureFormat::RGBA8Unorm; }

private:
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;

  // TEMPORARY 8a wgpu aliases; deleted in packet 8b (see GeodePipeline).
  wgpu::RenderPipeline borrowedPipeline_;
  wgpu::BindGroupLayout borrowedBindGroupLayout_;
};

/**
 * Compute pipeline for GPU-side snapshot unpremultiplication.
 *
 * Reads a premultiplied-alpha render target (binding 0, `texture_2d<f32>`)
 * and writes straight-alpha RGBA8 into the storage texture at binding 1
 * (`rgba8unorm`, write-only). Owned lazily by `GeodeDevice` so every
 * snapshot sharing the device reuses one compiled pipeline (issue #575:
 * wgpu-native retains compiled pipelines).
 *
 * The stored bytes replicate the CPU round-half-up reference formula in
 * `RendererGeode::ReadGeodeTextureSnapshot` exactly, so snapshots produced
 * through this pipeline are byte-identical to the CPU readback path.
 */
class GeodeSnapshotReadbackPipeline {
public:
  /**
   * Create the snapshot-unpremultiply compute pipeline for the given device.
   */
  explicit GeodeSnapshotReadbackPipeline(const wgpu::Device& device);

  ~GeodeSnapshotReadbackPipeline() = default;
  GeodeSnapshotReadbackPipeline(const GeodeSnapshotReadbackPipeline&) = delete;
  GeodeSnapshotReadbackPipeline& operator=(const GeodeSnapshotReadbackPipeline&) = delete;
  GeodeSnapshotReadbackPipeline(GeodeSnapshotReadbackPipeline&&) noexcept = default;
  GeodeSnapshotReadbackPipeline& operator=(GeodeSnapshotReadbackPipeline&&) noexcept = default;

  /// True when the bind group layout and compute pipeline compiled.
  bool valid() const { return static_cast<bool>(pipeline_) && static_cast<bool>(bindGroupLayout_); }

  /// The compiled compute pipeline.
  const wgpu::ComputePipeline& pipeline() const { return pipeline_.get(); }
  /// The bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_.get(); }

private:
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout_;
  ScopedWgpuHandle<wgpu::ComputePipeline> pipeline_;
};

}  // namespace donner::geode
