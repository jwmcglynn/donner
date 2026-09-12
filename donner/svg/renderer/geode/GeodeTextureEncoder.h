#pragma once
/// @file
/// Reusable GPU texture upload + textured-quad draw helpers for Geode.

#include <cstdint>
#include <optional>

#include "donner/base/Box.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Handles.h"
#include "donner/svg/renderer/geode/GeodeGpuContext.h"

namespace donner::geode {

class GeodeImagePipeline;

/**
 * Reusable helpers for uploading pixel data to a GPU texture and drawing it as a textured quad
 * through `GeodeImagePipeline`.
 *
 * This is the piece `drawImage` and pattern tile rendering share.
 * Both paths need:
 *   1. Get an RGBA8 pixel buffer to the GPU as a sampled texture.
 *   2. Draw it into an open render pass at a specific destination rectangle,
 *      honoring the current transform and an opacity factor.
 *
 * Pattern rendering calls `uploadRgba8Texture` to move a rendered
 * pattern tile into a sampled texture and then `drawTexturedQuad` to stamp
 * that tile across the target region (with `srcRect` chosen to implement
 * the `patternContentUnits` / `patternUnits` mapping).
 *
 * The class is pure helpers - no mutable state. It's a class rather than
 * free functions only to keep namespacing sensible.
 */
class GeodeTextureEncoder {
public:
  /**
   * Upload a tightly packed RGBA8 pixel buffer to a freshly created sampled texture. Alpha
   * interpretation is carried by the draw parameters.
   *
   * Texture writes carry a 256-byte row pitch. This function handles the case where the source
   * buffer's row stride (`width * 4`) is not 256-aligned by copying rows into a padded staging
   * buffer before the upload.
   *
   * Returns an empty texture if `width`, `height`, or `rgbaPixels` are invalid.
   *
   * @param context The recording context providing the GPU runtime device.
   * @param rgbaPixels RGBA8 pixels in row-major order.
   *   Must contain at least `width * height * 4` bytes.
   * @param width  Image width in pixels. Must be > 0.
   * @param height Image height in pixels. Must be > 0.
   */
  static gpu::Texture uploadRgba8Texture(const GeodeGpuContext& context, const uint8_t* rgbaPixels,
                                         uint32_t width, uint32_t height);

  /// Uniform-buffer binding offset alignment shared by the blit path and any UniformScratch
  /// implementation. Names \ref donner::geode::kUniformOffsetAlignment, which is the one
  /// definition; this member is the spelling the blit path and its tests already use.
  static constexpr uint64_t kUniformOffsetAlignment = geode::kUniformOffsetAlignment;

  /// Borrowed uniform-buffer range for one blit uniform upload. The
  /// `buffer` field is a raw non-owning handle: the provider keeps it
  /// alive (see UniformScratch's contract below); holders must not retain
  /// the allocation beyond the current frame.
  struct UniformAllocation {
    gpu::BufferRef buffer;
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  /**
   * Interface for callers that pool blit uniform uploads into a per-frame
   * scratch arena instead of creating one uniform buffer per blit.
   * `GeoEncoder` implements this over its per-encoder uniform arena, so
   * consecutive image blits and layer composites share one growing buffer
   * (the buffer itself is only created when the arena grows).
   *
   * Implementation contract (GeoEncoder's arena satisfies all four):
   * - The returned buffer carries the Uniform buffer-usage flag.
   * - The buffer outlives every command buffer recorded against it in the
   *   current frame (retire-then-release, never destroy mid-frame).
   * - The range `[offset, offset + size)` is never rewritten for the rest
   *   of the frame once returned.
   * - `offset` honors the requested alignment (at least
   *   kUniformOffsetAlignment for blit uniforms).
   */
  class UniformScratch {
  public:
    virtual ~UniformScratch() = default;

    /// Copy `size` bytes of uniform data into the scratch arena and return
    /// the borrowed (buffer, offset, size) range for the bind group.
    virtual UniformAllocation allocate(const void* data, uint64_t size, uint64_t alignment) = 0;
  };

  /// Filtering mode for `drawTexturedQuad`.
  enum class Filter : uint8_t {
    /// Bilinear filtering. Default for SVG `image-rendering: auto`.
    Linear,
    /// Nearest-neighbor filtering for `crisp-edges` and explicit pattern sampling.
    Nearest,
    /// Integer nearest-neighbor scaling followed by smooth scaling.
    Pixelated,
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
    /// Device-pixel scale per source texel for CSS `pixelated` sampling.
    double pixelatedScaleX = 1.0;
    /// @see pixelatedScaleX
    double pixelatedScaleY = 1.0;
    /// Set when the source texture already stores premultiplied-alpha pixels.
    /// `drawImage` premultiplies `ImageResource` pixels before upload, and
    /// offscreen render targets blitted during `popIsolatedLayer` / pattern
    /// compositing are already premultiplied. Both paths set this flag to avoid
    /// a second premultiplication that darkens the RGB channel. Straight-alpha
    /// callers leave the default false so the shader premultiplies on output.
    bool sourceIsPremultiplied = false;
    /// `<mask>` compositing input. Ignored unless `maskMode` is nonzero. Borrowed.
    const gpu::Texture* maskTexture = nullptr;
    /// Mask coverage selector: 0 disables masking, 1 uses luminance, and 2 uses alpha.
    uint32_t maskMode = 0;
    /// When true, output pixels outside `maskBounds` are discarded.
    /// Used to honour the `<mask>` element's x/y/width/height.
    bool applyMaskBounds = false;
    /// Mask bounds in target-pixel space. Ignored unless
    /// `applyMaskBounds` is true.
    Box2d maskBounds;
    /// SVG `mix-blend-mode` selector. `0` = plain
    /// source-over; `1..=16` map to the enumeration in
    /// `donner::svg::MixBlendMode` (Normal..Luminosity). When
    /// non-zero, `dstSnapshotTexture` must hold the parent render
    /// target's frozen content for the fragment shader to read as
    /// the backdrop.
    uint32_t blendMode = 0;
    /// Frozen snapshot of the parent render target - see
    /// `RendererGeode::popIsolatedLayer` which copies the prior
    /// parent content into a separate texture before opening the
    /// blend blit pass. Ignored unless `blendMode != 0`. Borrowed.
    const gpu::Texture* dstSnapshotTexture = nullptr;
    /// Path-clip mask view. When set, the image shader samples
    /// it in target-pixel space and gates the source content before any
    /// blend/mask compositing. Borrowed.
    const gpu::TextureView* clipMaskView = nullptr;
  };

  /**
   * Record a textured-quad draw call into an already-open render pass.
   *
   * The caller must:
   *   - Have already called `pass.setPipeline(pipeline.pipeline())` OR be
   *     OK with the draw switching the pipeline (we call SetPipeline
   *     internally to keep this helper self-contained).
   *   - Provide an MVP matrix built the same way as `GeoEncoder::Impl::buildMvp`
   *     (target-pixel → clip space, composed with the model-view transform).
   *
   * Allocates a bind group per call (retained in `resourceArena` until the
   * caller finishes the enclosing command encoder). The uniform upload
   * either goes into `scratch` when provided (one buffer create on arena
   * growth instead of one per blit) or into a fresh per-blit buffer on the
   * legacy path.
   *
   * @param resourceArena Scoped owner for GPU handles created by the draw.
   * @param scratch Optional pooled uniform scratch (see `UniformScratch`).
   */
  static void drawTexturedQuad(const GeodeGpuContext& context, const GeodeImagePipeline& pipeline,
                               gpu::RenderPassEncoder& pass, const gpu::Texture& texture,
                               const float mvp[16], uint32_t targetWidth, uint32_t targetHeight,
                               const QuadParams& params, GeodeTransientResources& resourceArena,
                               UniformScratch* scratch = nullptr);
};

}  // namespace donner::geode
