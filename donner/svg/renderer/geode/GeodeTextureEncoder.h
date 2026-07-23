#pragma once
/// @file
/// Reusable GPU texture upload + textured-quad draw helpers for Geode.

#include <cstdint>
#include <optional>

#include "donner/base/Box.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/svg/renderer/geode/GeodeGpuContext.h"

namespace donner::geode {

class GeodeImagePipeline;

/**
 * Reusable helpers for uploading pixel data to a `gpu::Texture` and drawing
 * it as a textured quad through `GeodeImagePipeline`.
 *
 * This is the piece `drawImage` and pattern tile rendering share.
 * Both paths need:
 *   1. Get an RGBA8 pixel buffer to the GPU as a sampled texture.
 *   2. Draw it into an open render pass at a specific destination rectangle,
 *      honoring the current transform and an opacity factor.
 *
 * The class is pure helpers - no mutable state. It's a class rather than
 * free functions only to keep namespacing sensible.
 */
class GeodeTextureEncoder {
public:
  /**
   * Upload a tightly packed straight-alpha RGBA8 pixel buffer to a freshly
   * created `gpu::Texture` (usage = `Sampled | CopyDst`).
   *
   * The donner::gpu runtime requires `bytesPerRow` to be 256-aligned for
   * texture writes. When the source buffer's row stride (`width * 4`) is
   * already 256-aligned the rows upload directly; otherwise rows are copied
   * into a padded staging buffer before the upload.
   *
   * Returns a null texture handle if `width`, `height`, or `rgbaPixels`
   * are invalid, or if the creation fails.
   *
   * @param context The Geode GPU recording context (provides the
   *   `gpu::Device` uploads go through).
   * @param rgbaPixels Straight-alpha RGBA8 pixels in row-major order.
   *   Must contain at least `width * height * 4` bytes.
   * @param width  Image width in pixels. Must be > 0.
   * @param height Image height in pixels. Must be > 0.
   */
  static gpu::Texture uploadRgba8Texture(const GeodeGpuContext& context, const uint8_t* rgbaPixels,
                                         uint32_t width, uint32_t height);

  /// Filtering mode for `drawTexturedQuad`.
  enum class Filter : uint8_t {
    /// Bilinear filtering. Default for SVG `image-rendering: auto`.
    Linear,
    /// Nearest-neighbor filtering. Used for `image-rendering: pixelated`
    /// (and for pattern tile sampling where the host explicitly opts in).
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
    /// Source UV rectangle in [0,1] x [0,1]. Default = entire texture.
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
    /// `RendererGeode::popMask` sets it. Non-owning; the caller keeps
    /// the texture alive through the frame's submit.
    const gpu::Texture* maskTexture = nullptr;
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
    /// Frozen snapshot of the parent render target - see
    /// `RendererGeode::popIsolatedLayer` which copies the prior
    /// parent content into a separate texture before opening the
    /// blend blit pass. Ignored unless `blendMode != 0`. Non-owning;
    /// the caller keeps the texture alive through the frame's submit.
    const gpu::Texture* dstSnapshotTexture = nullptr;
    /// Phase 3b path-clip mask view. When valid, the image shader samples
    /// it in target-pixel space and gates the source content before any
    /// blend/mask compositing. The referenced view handle must stay alive
    /// through the frame's submit.
    gpu::TextureViewRef clipMaskView;
  };

  /**
   * Record a textured-quad draw call into an already-open render pass.
   *
   * The caller must:
   *   - Have already called `pass.setPipeline(pipeline.pipeline())` OR be
   *     OK with the draw switching the pipeline (we call setPipeline
   *     internally to keep this helper self-contained).
   *   - Provide an MVP matrix built the same way as `GeoEncoder::Impl::buildMvp`
   *     (target-pixel -> clip space, composed with the model-view transform).
   *
   * Allocates fresh GPU resources (uniform buffer + texture views + bind
   * group) per call and retains them in `transients` until the caller
   * submits the enclosing command buffer (mandatory: `gpu::Device::submit`
   * fails closed on destroyed recorded resources).
   *
   * @param context The Geode GPU recording context.
   * @param pipeline The image-blit pipeline the quad draws through.
   * @param pass The already-open render pass to record into.
   * @param texture Source content texture (must carry `Sampled` usage).
   * @param mvp Column-major model-view-projection matrix.
   * @param targetWidth Render target width in pixels.
   * @param targetHeight Render target height in pixels.
   * @param params Quad parameters.
   * @param transients Keepalive for handles created by the draw.
   */
  static void drawTexturedQuad(const GeodeGpuContext& context, const GeodeImagePipeline& pipeline,
                               gpu::RenderPassEncoder& pass, const gpu::Texture& texture,
                               const float mvp[16], uint32_t targetWidth, uint32_t targetHeight,
                               const QuadParams& params, GeodeTransientResources& transients);
};

}  // namespace donner::geode
