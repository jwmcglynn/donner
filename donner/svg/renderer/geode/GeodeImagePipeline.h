#pragma once
/// @file
/// Render pipeline for the image-blit shader (textured quad).

#include <webgpu/webgpu.hpp>

#include "donner/gpu/Device.h"

namespace donner::geode {

class GeodeWgpuAdapterDevice;

/**
 * Caches a compiled render pipeline for the image-blit shader plus its bind group layout and
 * two reusable samplers (linear and nearest).
 *
 * One `GeodeImagePipeline` is sufficient per `(device, render-target-format)`
 * pair. It is used both by `drawImage` (SVG `<image>` elements) and, in
 * Phase 2H, by the pattern renderer: the pattern tile is rendered to an
 * offscreen texture and then sampled with this same pipeline as a
 * repeating fill.
 *
 * The pipeline and samplers are created through the \c donner::gpu runtime (design 0053
 * packet 8): the class holds the RAII `donner::gpu` handles, and - TEMPORARY for 8a while
 * GeoEncoder / GeodeTextureEncoder still record through wgpu - caches the borrowed wgpu objects
 * resolved through the adapter's escape hatches (deleted in packet 8b).
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
   * @param adapterDevice The Donner GPU device (wgpu adapter) owned by the GeodeDevice.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   */
  GeodeImagePipeline(GeodeWgpuAdapterDevice& adapterDevice, gpu::TextureFormat colorFormat);

  ~GeodeImagePipeline() = default;
  GeodeImagePipeline(const GeodeImagePipeline&) = delete;
  GeodeImagePipeline& operator=(const GeodeImagePipeline&) = delete;
  /// Move constructor.
  GeodeImagePipeline(GeodeImagePipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeImagePipeline& operator=(GeodeImagePipeline&&) noexcept = default;

  /// The compiled render pipeline (TEMPORARY 8a wgpu alias for the still-wgpu encoders).
  const wgpu::RenderPipeline& pipeline() const { return borrowedPipeline_; }

  /// Bind group layout used by the pipeline (TEMPORARY 8a wgpu alias).
  const wgpu::BindGroupLayout& bindGroupLayout() const { return borrowedBindGroupLayout_; }

  /// Bilinear (mag/min filter = Linear) sampler, clamped to edge.
  /// Used for the default `image-rendering` and the SVG spec's
  /// "smooth" image sampling. (TEMPORARY 8a wgpu alias.)
  const wgpu::Sampler& linearSampler() const { return borrowedLinearSampler_; }

  /// Nearest-neighbor sampler, clamped to edge. Used when
  /// `ImageParams::imageRenderingPixelated` is true. (TEMPORARY 8a wgpu alias.)
  const wgpu::Sampler& nearestSampler() const { return borrowedNearestSampler_; }

  /// Linear clamp-to-edge sampler used for Phase 3b clip-mask textures.
  /// (TEMPORARY 8a wgpu alias.)
  const wgpu::Sampler& clipMaskSampler() const { return borrowedClipMaskSampler_; }

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
  gpu::Sampler clipMaskSampler_;

  // TEMPORARY 8a: borrowed wgpu aliases resolved through the adapter's escape hatches so the
  // still-wgpu encoders can bind them. Deleted in packet 8b.
  wgpu::RenderPipeline borrowedPipeline_;
  wgpu::BindGroupLayout borrowedBindGroupLayout_;
  wgpu::Sampler borrowedLinearSampler_;
  wgpu::Sampler borrowedNearestSampler_;
  wgpu::Sampler borrowedClipMaskSampler_;
};

}  // namespace donner::geode
