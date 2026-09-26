#pragma once
/// @file
/// Render pipeline for the Slug fill algorithm.

#include <string_view>

#include "donner/gpu/Device.h"

namespace donner::geode {

/**
 * Caches a compiled render pipeline for the Slug fill shader, plus its bind group layout.
 *
 * One `GeodePipeline` instance is sufficient per `(device, render-target-format)`
 * pair - the actual data (uniforms, bands, curves) varies per
 * draw call but the pipeline state object can be reused.
 *
 * The pipeline is created through the \c donner::gpu runtime, which owns it through RAII
 *
 * Resource bindings and stage visibility come from the compiled Slug interface.
 *
 * The pipeline has no vertex buffer. Its shader expands the uniform bounding polygon into a
 * triangle fan from `vertex_index` and applies the half-pixel AA halo in device space.
 */
class GeodePipeline {
public:
  /**
   * Create a Slug fill pipeline for the given device and color target format.
   *
   * @param device Runtime device the pipeline and its layouts are created on.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodePipeline(gpu::Device& device, gpu::TextureFormat colorFormat);

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
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }

  /**
   * The cross-entity batch variant of \ref donner::geode::GeodePipeline::pipeline "pipeline": same
   * layout, same shader module and same blending, but the entry points that take paint and geometry
   * from each instance's record. Compiled on first call, because only a cross-entity batch needs
   * it.
   */
  const gpu::RenderPipeline& batchedPipeline() const;

  /// The bind group layout used by both pipelines.
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }

  /// Color format the pipeline was built for.
  gpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  /// Compile one variant of the Slug fill pipeline through the GPU runtime. Both variants share
  /// the layout, the shader module and the blend state; only the entry points differ.
  gpu::RenderPipeline buildPipeline(const char* label, std::string_view vertexEntryPoint,
                                    std::string_view fragmentEntryPoint) const;

  /// The device both pipeline variants are created through. Owned by the GeodeDevice that owns
  /// this pipeline, so it outlives every use here.
  gpu::Device* device_ = nullptr;
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
  /// @param device Runtime device the pipeline and its layouts are created on.
  /// @param colorFormat Pixel format of the render target this pipeline draws into.
  GeodeGradientPipeline(gpu::Device& device, gpu::TextureFormat colorFormat);

  ~GeodeGradientPipeline() = default;
  GeodeGradientPipeline(const GeodeGradientPipeline&) = delete;
  GeodeGradientPipeline& operator=(const GeodeGradientPipeline&) = delete;
  /// Move constructor.
  GeodeGradientPipeline(GeodeGradientPipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeGradientPipeline& operator=(GeodeGradientPipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }
  /// The bind group layout used by the pipeline.
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
 * (`donner/gpu/shader/programs/SlugMaskSource.h`) plus its bind-group layout.
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
   *
   * @param device Runtime device the pipeline and its layouts are created on.
   */
  explicit GeodeMaskPipeline(gpu::Device& device);

  ~GeodeMaskPipeline() = default;
  GeodeMaskPipeline(const GeodeMaskPipeline&) = delete;
  GeodeMaskPipeline& operator=(const GeodeMaskPipeline&) = delete;

  /// Construct by moving another instance's state.
  /// @param other Source object.
  GeodeMaskPipeline(GeodeMaskPipeline&& other) noexcept = default;

  /// Replace this object's state by moving another instance.
  /// @param other Source object.
  /// @return This object after the move.
  GeodeMaskPipeline& operator=(GeodeMaskPipeline&& other) noexcept = default;

  /// The compiled render pipeline.
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }
  /// The bind group layout used by the pipeline.
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }
  /// The color format the pipeline targets. Always `RGBA8Unorm`.
  gpu::TextureFormat colorFormat() const { return gpu::TextureFormat::RGBA8Unorm; }

private:
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;
};

/**
 * Compute pipeline for GPU-side snapshot unpremultiplication.
 *
 * Reads a premultiplied-alpha render target (`texture_2d<f32>`) and writes
 * straight-alpha RGBA8 into a write-only `rgba8unorm` storage texture; both
 * binding slots and the workgroup shape come from the precompiled artifact's
 * reflected interface. Owned lazily by `GeodeDevice` so every
 * snapshot sharing the device reuses one compiled pipeline (issue #575:
 * wgpu-native retains compiled pipelines).
 *
 * The stored bytes replicate the CPU round-half-up reference formula in
 * `RendererGeode::ReadGeodeTextureSnapshot` exactly, so snapshots produced
 * through this pipeline are byte-identical to the CPU readback path.
 */
class GeodeSnapshotReadbackPipeline {
public:
  /// Create the snapshot-unpremultiply compute pipeline on \p device from the precompiled
  /// artifact.
  /// @param device Runtime device the pipeline and its layouts are created on.
  explicit GeodeSnapshotReadbackPipeline(gpu::Device& device);

  ~GeodeSnapshotReadbackPipeline() = default;
  GeodeSnapshotReadbackPipeline(const GeodeSnapshotReadbackPipeline&) = delete;
  GeodeSnapshotReadbackPipeline& operator=(const GeodeSnapshotReadbackPipeline&) = delete;

  /// Construct by moving another instance's state.
  /// @param other Source object.
  GeodeSnapshotReadbackPipeline(GeodeSnapshotReadbackPipeline&& other) noexcept = default;

  /// Replace this object's state by moving another instance.
  /// @param other Source object.
  /// @return This object after the move.
  GeodeSnapshotReadbackPipeline& operator=(GeodeSnapshotReadbackPipeline&& other) noexcept =
      default;

  /// True when the bind group layout and compute pipeline were created.
  bool valid() const { return pipeline_.isValid() && bindGroupLayout_.isValid(); }

  /// The compute pipeline.
  const gpu::ComputePipeline& pipeline() const { return pipeline_; }
  /// The bind group layout used by the pipeline.
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }
  /// Reflected binding of the premultiplied source texture.
  uint32_t inputBinding() const { return inputBinding_; }
  /// Reflected binding of the straight-alpha storage destination.
  uint32_t outputBinding() const { return outputBinding_; }
  /// Workgroup shape the entry point declares.
  gpu::WorkgroupSize workgroupSize() const { return workgroupSize_; }

private:
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::ComputePipeline pipeline_;
  uint32_t inputBinding_ = 0;
  uint32_t outputBinding_ = 1;
  gpu::WorkgroupSize workgroupSize_{8, 8, 1};
};

}  // namespace donner::geode
