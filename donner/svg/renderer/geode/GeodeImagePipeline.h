#pragma once
/// @file
/// Render pipeline for the image-blit shader (textured quad).

#include "donner/gpu/Device.h"

namespace donner::geode {

/**
 * Caches a compiled render pipeline for the image-blit shader plus its bind group layout and
 * two reusable samplers (linear and nearest).
 *
 * One `GeodeImagePipeline` is sufficient per `(device, render-target-format)`
 * pair. It is used both by `drawImage` (SVG `<image>` elements) and by the
 * pattern renderer: the pattern tile is rendered to an
 * offscreen texture and then sampled with this same pipeline as a
 * repeating fill.
 *
 * The pipeline and samplers are created through the \c donner::gpu runtime, which owns them
 * through RAII handles.
 *
 * Resource bindings and entry names come from the compiled shader interface. Optional texture
 * inputs receive valid placeholder views when their corresponding feature is disabled.
 *
 * The pipeline takes no vertex buffer - the shader generates quad corners
 * from `@builtin(vertex_index)`. A draw call is `pass.Draw(6, 1, 0, 0)`.
 */
class GeodeImagePipeline {
public:
  /**
   * Create an image-blit pipeline for the given device and target format.
   *
   * @param device Runtime device the pipeline, its layouts, and its samplers are created on.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodeImagePipeline(gpu::Device& device, gpu::TextureFormat colorFormat);

  ~GeodeImagePipeline() = default;
  GeodeImagePipeline(const GeodeImagePipeline&) = delete;
  GeodeImagePipeline& operator=(const GeodeImagePipeline&) = delete;
  /// Move constructor.
  GeodeImagePipeline(GeodeImagePipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeImagePipeline& operator=(GeodeImagePipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const gpu::RenderPipeline& pipeline() const { return pipeline_; }

  /// Bind group layout used by the pipeline.
  const gpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }

  /// Bilinear (mag/min filter = Linear) sampler, clamped to edge. Used for the default
  /// `image-rendering` and the SVG spec's "smooth" image sampling.
  const gpu::Sampler& linearSampler() const { return linearSampler_; }

  /// Nearest-neighbor sampler, clamped to edge. Used when
  /// `ImageRendering::CrispEdges` or its legacy alias is selected.
  const gpu::Sampler& nearestSampler() const { return nearestSampler_; }

  /// Color format the pipeline was built for.
  gpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  gpu::TextureFormat colorFormat_ = gpu::TextureFormat::RGBA8Unorm;
  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline pipeline_;
  gpu::Sampler linearSampler_;
  gpu::Sampler nearestSampler_;
};

}  // namespace donner::geode
