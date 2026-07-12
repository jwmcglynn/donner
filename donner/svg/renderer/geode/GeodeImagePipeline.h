#pragma once
/// @file
/// Render pipeline for the image-blit shader (textured quad).

#include <webgpu/webgpu.hpp>

#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

/**
 * Caches a compiled `wgpu::RenderPipeline` for the image-blit shader plus its
 * bind group layout and two reusable samplers (linear and nearest).
 *
 * One `GeodeImagePipeline` is sufficient per `(device, render-target-format)`
 * pair. It is used both by `drawImage` (SVG `<image>` elements) and, in
 * Phase 2H, by the pattern renderer: the pattern tile is rendered to an
 * offscreen texture and then sampled with this same pipeline as a
 * repeating fill.
 *
 * Bind group layout (matches `shaders/image_blit.wgsl`):
 * - binding 0: uniform buffer (mvp, destRect, srcRect, targetSize, opacity, flags)
 * - binding 1: sampler (filter mode chosen at draw time)
 * - binding 2: sampled texture 2D (float)
 * - binding 3: sampled mask texture for `<mask>` luminance mode
 * - binding 4: sampled destination snapshot for `mix-blend-mode`
 * - binding 5: sampled Phase 3b clip-mask texture
 * - binding 6: clip-mask sampler (always linear clamp-to-edge)
 *
 * The pipeline takes no vertex buffer - the shader generates quad corners
 * from `@builtin(vertex_index)`. A draw call is `pass.Draw(6, 1, 0, 0)`.
 */
class GeodeImagePipeline {
public:
  /**
   * Create an image-blit pipeline for the given device and target format.
   *
   * @param device The WebGPU device.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodeImagePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat);

  ~GeodeImagePipeline() = default;
  GeodeImagePipeline(const GeodeImagePipeline&) = delete;
  GeodeImagePipeline& operator=(const GeodeImagePipeline&) = delete;
  /// Move constructor.
  GeodeImagePipeline(GeodeImagePipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeImagePipeline& operator=(GeodeImagePipeline&&) noexcept = default;

  /// The compiled render pipeline.
  const wgpu::RenderPipeline& pipeline() const { return pipeline_.get(); }

  /// Bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_.get(); }

  /// Bilinear (mag/min filter = Linear) sampler, clamped to edge.
  /// Used for the default `image-rendering` and the SVG spec's
  /// "smooth" image sampling.
  const wgpu::Sampler& linearSampler() const { return linearSampler_.get(); }

  /// Nearest-neighbor sampler, clamped to edge. Used when
  /// `ImageParams::imageRenderingPixelated` is true.
  const wgpu::Sampler& nearestSampler() const { return nearestSampler_.get(); }

  /// Linear clamp-to-edge sampler used for Phase 3b clip-mask textures.
  const wgpu::Sampler& clipMaskSampler() const { return clipMaskSampler_.get(); }

  /// Color format the pipeline was built for.
  wgpu::TextureFormat colorFormat() const { return colorFormat_; }

private:
  wgpu::TextureFormat colorFormat_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout_;
  ScopedWgpuHandle<wgpu::RenderPipeline> pipeline_;
  ScopedWgpuHandle<wgpu::Sampler> linearSampler_;
  ScopedWgpuHandle<wgpu::Sampler> nearestSampler_;
  ScopedWgpuHandle<wgpu::Sampler> clipMaskSampler_;
};

}  // namespace donner::geode
