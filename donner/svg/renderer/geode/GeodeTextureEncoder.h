#pragma once
/// @file
/// Reusable GPU texture upload + textured-quad draw helpers for Geode.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Handles.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
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
   * consecutive image blits, and layer composites without a retained binding, share one growing
   * buffer (the buffer itself is only created when the arena grows).
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
    /// Bilinear filtering, for `image-rendering: smooth` and layer or pattern composites.
    Linear,
    /// The texel containing each sample, for `crisp-edges`, translation-only image draws and
    /// explicit pattern sampling.
    Nearest,
    /// Integer nearest-neighbor scaling followed by smooth scaling.
    Pixelated,
    /// Mitchell-Netravali cubic (B = C = 1/3) over premultiplied texels, for `<image>` quality
    /// rendering. Uses the linear sampler only for mask and backdrop reads.
    Cubic,
    /// Bilinear filtering through a texel mapping split at `QuadParams::anchor`, for resamples
    /// that must not change when the target and the content move together by whole pixels.
    Anchored,
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
    /// Maps `destRect`'s space to target pixels; the MVP must apply the same mapping. Nearest
    /// sampling inverts it to find the source texel under each pixel center.
    Transform2d targetFromLocal;
    /// Overall opacity multiplier in [0, 1]. Combined with the sampled
    /// alpha inside the fragment shader.
    double opacity = 1.0;
    /// Sampling filter mode.
    Filter filter = Filter::Linear;
    /// Target pixel that the content stays aligned to for `Filter::Anchored`: two draws whose
    /// source and target differ by whole pixels, with the anchor at the same target pixel, sample
    /// corresponding pixels with the same bilinear weights.
    Vector2i anchor = Vector2i::Zero();
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
    /// Mask coverage selector: 0 disables masks, 1 uses sRGB luminance, 2 alpha, 3 linear
    /// luminance.
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
   * Uniform buffer and bind group that one textured quad keeps across frames, so drawing it again
   * with unchanged inputs writes no buffer and creates no bind group.
   *
   * The bind group is reused only with the exact textures it was built from. Each one is matched
   * by its runtime identity, a slot plus a generation that no later texture ever takes, so a
   * reused bind group cannot sample a texture that was destroyed, nor one whose slot now holds a
   * different texture. A texture returned to a pool and handed out again keeps its identity, so a
   * caller must only pass a binding when the quad's current owner rendered the texture's contents
   * for this draw. The uniform buffer is rewritten only when the quad's uniform bytes change.
   *
   * A binding serves at most one draw per frame: its uniform write reaches the queue ahead of the
   * frame's command buffers, so a second draw recorded in the same frame would make the first read
   * the second's bytes.
   */
  class RetainedQuadBinding {
  public:
    /// GPU bytes one binding keeps: its uniform buffer. It mirrors the same number of bytes on the
    /// CPU to detect unchanged uniforms.
    static constexpr uint64_t kRetainedBytes = sizeof(gpu::shader::programs::ImageBlitParams);

    RetainedQuadBinding() = default;
    RetainedQuadBinding(const RetainedQuadBinding&) = delete;
    RetainedQuadBinding& operator=(const RetainedQuadBinding&) = delete;
    /// Move constructor.
    RetainedQuadBinding(RetainedQuadBinding&&) noexcept = default;
    /// Move assignment.
    RetainedQuadBinding& operator=(RetainedQuadBinding&&) noexcept = default;

    /// Whether this binding holds a texture view or a bind group naming one.
    [[nodiscard]] bool holdsTextureViews() const {
      return sourceView_.isValid() || backdropView_.isValid() || bindGroup_.isValid();
    }

    /// Whether this binding holds a view of the texture \p texture, as its source or backdrop.
    /// @param texture Runtime identity of the texture.
    [[nodiscard]] bool viewsTexture(const gpu::ResourceIdentity& texture) const;

    /// Whether this binding's bind group names a clip mask view. The binding does not own that
    /// view, so the bind group must not outlive the frame that supplied it.
    [[nodiscard]] bool bindsClipMask() const;

    /**
     * Release the texture views and the bind group, keeping the uniform buffer and the bytes it
     * holds. The next draw through this binding rebuilds them. Call it before a texture this
     * binding views is destroyed: on Vulkan a view is a separate object on the image, and nothing
     * else releases it with the image.
     */
    void releaseTextureViews();

  private:
    friend class GeodeTextureEncoder;

    /// What a bind group is built from. A null identity stands for an absent input.
    struct Inputs {
      gpu::ResourceIdentity layout;
      gpu::ResourceIdentity source;
      gpu::ResourceIdentity backdrop;
      gpu::ResourceIdentity clipMask;
      uint64_t deviceId = 0;
      bool nearestSampler = false;

      /// Equality operator. @param other Inputs to compare against.
      bool operator==(const Inputs& other) const = default;
    };

    /**
     * Make the uniform buffer hold \p bytes, creating it on first use and writing only when its
     * contents differ.
     *
     * @param device Device the buffer lives on.
     * @param bytes Uniform block for this draw.
     * @return False when the buffer could not be created or written.
     */
    bool publishUniforms(gpu::Device& device, std::span<const uint8_t> bytes);

    /**
     * Build the bind group for \p inputs, with views of \p texture and of the quad's backdrop.
     *
     * @param device Device to create the views and bind group on.
     * @param pipeline Image pipeline whose layout the bind group follows.
     * @param texture Source texture sampled by the quad.
     * @param params Quad parameters naming the backdrop and clip mask.
     * @param inputs Identities of what the bind group binds.
     * @return False, leaving no bind group, when the device refused a view or the bind group.
     */
    bool rebuildBindGroup(gpu::Device& device, const GeodeImagePipeline& pipeline,
                          const gpu::Texture& texture, const QuadParams& params,
                          const Inputs& inputs);

    gpu::Buffer uniformBuffer_;
    std::array<uint8_t, kRetainedBytes> uniformBytes_ = {};
    /// Whether \ref uniformBytes_ holds what the GPU buffer holds.
    bool uniformCurrent_ = false;
    /// Whether \ref bindGroup_ binds the current \ref uniformBuffer_.
    bool bindGroupBindsBuffer_ = false;
    gpu::TextureView sourceView_;
    gpu::TextureView backdropView_;
    gpu::BindGroup bindGroup_;
    /// Inputs \ref bindGroup_ was built from.
    Inputs inputs_;
  };
  static_assert(!std::is_copy_constructible_v<RetainedQuadBinding> &&
                    std::is_nothrow_move_constructible_v<RetainedQuadBinding> &&
                    std::is_nothrow_move_assignable_v<RetainedQuadBinding>,
                "Containers of bindings must move them, never copy their GPU handles.");

  /**
   * Build the column-major clip-from-local matrix for a quad drawn through \p targetFromLocal
   * into a \p targetWidth x \p targetHeight target (y down in target pixels, up in clip space).
   *
   * @param targetFromLocal Transform from the quad's local space to target pixels.
   * @param targetWidth Render-target width in pixels.
   * @param targetHeight Render-target height in pixels.
   * @param out16 Receives the 16 matrix entries.
   */
  static void BuildClipFromLocal(const Transform2d& targetFromLocal, uint32_t targetWidth,
                                 uint32_t targetHeight, float out16[16]);

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
   * @param context GPU context used to allocate draw resources.
   * @param pipeline Image pipeline compatible with the active target.
   * @param pass Open render pass receiving the draw.
   * @param texture Source texture sampled by the quad.
   * @param mvp Column-major 4x4 transform from quad coordinates to clip space.
   * @param targetWidth Render-target width in physical pixels.
   * @param targetHeight Render-target height in physical pixels.
   * @param params Quad placement, sampling, clipping, and compositing settings.
   */
  static void drawTexturedQuad(const GeodeGpuContext& context, const GeodeImagePipeline& pipeline,
                               gpu::RenderPassEncoder& pass, const gpu::Texture& texture,
                               const float mvp[16], uint32_t targetWidth, uint32_t targetHeight,
                               const QuadParams& params, GeodeTransientResources& resourceArena,
                               UniformScratch* scratch = nullptr);

  /**
   * Record a textured-quad draw through \p binding, reusing its uniform buffer and bind group
   * when they still match this draw (see \ref RetainedQuadBinding).
   *
   * Takes the same draw inputs as \ref drawTexturedQuad. Quads with a mask texture keep using
   * \ref drawTexturedQuad. Returns false without recording anything when the binding cannot serve
   * the draw (a mask texture, or a buffer, view or bind group the device refused); the caller then
   * draws through \ref drawTexturedQuad.
   *
   * @param context GPU context used to allocate draw resources.
   * @param pipeline Image pipeline compatible with the active target.
   * @param pass Open render pass receiving the draw.
   * @param texture Source texture sampled by the quad.
   * @param mvp Column-major 4x4 transform from quad coordinates to clip space.
   * @param targetWidth Render-target width in physical pixels.
   * @param targetHeight Render-target height in physical pixels.
   * @param params Quad placement, sampling, clipping, and compositing settings.
   * @param binding Binding this quad keeps across frames.
   * @return Whether the draw was recorded.
   */
  static bool drawRetainedQuad(const GeodeGpuContext& context, const GeodeImagePipeline& pipeline,
                               gpu::RenderPassEncoder& pass, const gpu::Texture& texture,
                               const float mvp[16], uint32_t targetWidth, uint32_t targetHeight,
                               const QuadParams& params, RetainedQuadBinding& binding);
};

}  // namespace donner::geode
