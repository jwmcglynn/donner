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
 * - binding 0: uniform buffer (Uniforms struct: mvp, viewport, color, fillRule)
 * - binding 1: storage buffer (read-only) — Band[]
 * - binding 2: storage buffer (read-only) — curve data (flat f32[])
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
  GeodePipeline(GeodePipeline&&) noexcept = default;
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

}  // namespace donner::geode
