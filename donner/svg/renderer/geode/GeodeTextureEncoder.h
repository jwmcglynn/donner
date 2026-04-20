#pragma once
/// @file
/// Reusable GPU texture upload + textured-quad draw helpers for Geode.

#include <webgpu/webgpu.hpp>

#include <cstdint>
#include <optional>

#include "donner/base/Box.h"

namespace donner::geode {

class GeodeDevice;
class GeodeImagePipeline;

/**
 * Reusable helpers for uploading pixel data to a `wgpu::Texture` and drawing
 * it as a textured quad through `GeodeImagePipeline`.
 *
 * This is the piece `drawImage` and (Phase 2H) pattern tile rendering share.
 * Both paths need:
 *   1. Get an RGBA8 pixel buffer to the GPU as a sampled texture.
 *   2. Draw it into an open render pass at a specific destination rectangle,
 *      honoring the current transform and an opacity factor.
 *
 * Phase 2H specifically will call `uploadRgba8Texture` to move a rendered
 * pattern tile into a sampled texture and then `drawTexturedQuad` to stamp
 * that tile across the target region (with `srcRect` chosen to implement
 * the `patternContentUnits` / `patternUnits` mapping).
 *
 * The class is pure helpers — no mutable state. It's a class rather than
 * free functions only to keep namespacing sensible.
 */
class GeodeTextureEncoder {
public:
  /**
   * Upload a tightly packed straight-alpha RGBA8 pixel buffer to a freshly
   * created `wgpu::Texture` (usage = `TextureBinding | CopyDst`).
   *
   * WebGPU requires `bytesPerRow` to be 256-aligned for texture writes, but
   * the unpadded-row path is exercised by `queue.WriteTexture` on all Dawn
   * backends regardless of format. This function handles the rare case
   * where the source buffer's row stride (`width * 4`) is not 256-aligned by
   * copying rows into a padded staging buffer before the upload.
   *
   * Returns an empty texture if `device`, `width`, `height`, or `rgbaPixels`
   * are invalid.
   *
   * @param device The Geode device wrapper (provides both `wgpu::Device`
   *   and `wgpu::Queue`).
   * @param rgbaPixels Straight-alpha RGBA8 pixels in row-major order.
   *   Must contain at least `width * height * 4` bytes.
   * @param width  Image width in pixels. Must be > 0.
   * @param height Image height in pixels. Must be > 0.
   */
  static wgpu::Texture uploadRgba8Texture(GeodeDevice& device, const uint8_t* rgbaPixels,
                                          uint32_t width, uint32_t height);

  /// Filtering mode for `drawTexturedQuad`.
  enum class Filter : uint8_t {
    /// Bilinear filtering. Default for SVG `image-rendering: auto`.
    Linear,
    /// Nearest-neighbor filtering. Used for `image-rendering: pixelated`
    /// (and, when Phase 2H lands, for pattern tile sampling where the
    /// host explicitly opts in).
    Nearest,
  };

  /**
   * Parameters for a single textured-quad draw call.
   *
   * `destRect` is in target-pixel space; the caller must bake the current
   * model-view transform into `targetFromLocal` if the rect is authored in
   * a different coordinate system. This keeps `drawTexturedQuad` agnostic
   * about where the MVP lives in the larger renderer's state stack.
   */
  struct QuadParams {
    /// Destination rectangle (in target-pixel space after applying
    /// `targetFromLocal`).
    Box2d destRect;
    /// Source UV rectangle in [0,1] × [0,1]. Default = entire texture.
    Box2d srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
    /// Overall opacity multiplier in [0, 1]. Combined with the sampled
    /// alpha inside the fragment shader.
    double opacity = 1.0;
    /// Sampling filter mode.
    Filter filter = Filter::Linear;
    /// Set when the source texture already stores premultiplied-alpha
    /// pixels. `drawImage` uses straight-alpha textures uploaded from
    /// `ImageResource` (default = false). Offscreen render targets that
    /// Geode blits back during `popIsolatedLayer` / pattern compositing
    /// are premultiplied and must set this flag to avoid a double
    /// premultiplication that darkens the RGB channel.
    bool sourceIsPremultiplied = false;
    /// Phase 3c `<mask>` luminance compositing. When non-null, this
    /// texture is sampled alongside the source and its BT.709
    /// luminance (multiplied by alpha, to match tiny-skia's
    /// `Mask::fromPixmap(Luminance)`) is used as a coverage
    /// multiplier on the output. Ignored unless
    /// `RendererGeode::popMask` sets it.
    wgpu::Texture maskTexture;
    /// When true, output pixels outside `maskBounds` are discarded.
    /// Used to honour the `<mask>` element's x/y/width/height.
    bool applyMaskBounds = false;
    /// Mask bounds in target-pixel space. Ignored unless
    /// `applyMaskBounds` is true.
    Box2d maskBounds;
    /// Phase 3d SVG `mix-blend-mode` selector. `0` = plain
    /// source-over; `1..=16` map to the enumeration in
    /// `donner::svg::MixBlendMode` (Normal..Luminosity). When
    /// non-zero, `dstSnapshotTexture` must hold the parent render
    /// target's frozen content for the fragment shader to read as
    /// the backdrop.
    uint32_t blendMode = 0;
    /// Frozen snapshot of the parent render target — see
    /// `RendererGeode::popIsolatedLayer` which copies the prior
    /// parent content into a separate texture before opening the
    /// blend blit pass. Ignored unless `blendMode != 0`.
    wgpu::Texture dstSnapshotTexture;
    /// Phase 3b path-clip mask view. When set, the image shader samples
    /// it in target-pixel space and gates the source content before any
    /// blend/mask compositing.
    wgpu::TextureView clipMaskView;
  };

  /**
   * Record a textured-quad draw call into an already-open render pass.
   *
   * The caller must:
   *   - Have already called `pass.SetPipeline(pipeline.pipeline())` OR be
   *     OK with the draw switching the pipeline (we call SetPipeline
   *     internally to keep this helper self-contained).
   *   - Provide an MVP matrix built the same way as `GeoEncoder::Impl::buildMvp`
   *     (target-pixel → clip space, composed with the model-view transform).
   *
   * Allocates fresh GPU resources (uniform buffer + bind group) per call —
   * caching is a Phase 5 concern.
   */
  static void drawTexturedQuad(GeodeDevice& device, const GeodeImagePipeline& pipeline,
                               const wgpu::RenderPassEncoder& pass, const wgpu::Texture& texture,
                               const float mvp[16], uint32_t targetWidth, uint32_t targetHeight,
                               const QuadParams& params);
};

}  // namespace donner::geode
