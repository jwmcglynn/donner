#pragma once
/// @file
/// Drawing API for the Geode GPU renderer.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Handles.h"
#include "donner/svg/core/ColorInterpolation.h"
#include "donner/svg/core/ImageRendering.h"
#include "donner/svg/core/MaskType.h"
#include "donner/svg/renderer/geode/GeodeResidentPathComponent.h"
#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"

namespace donner {
class Path;
}  // namespace donner

namespace donner::svg {
struct ImageResource;
}  // namespace donner::svg

namespace donner::geode {

struct EncodedPath;
struct GeodeResidentSlot;
class GeodeRecordSlab;
struct GeodeResidentGradientSlot;
struct GeodeGradientRamp;

/**
 * Optional debug observer for Slug draws that actually reach a GPU draw call.
 *
 * The observer is null in normal rendering. Implementations must copy any
 * needed `EncodedPath` data before returning because an inline encode may be
 * destroyed immediately after the submission call.
 */
class GeometryDebugSink {
public:
  virtual ~GeometryDebugSink() = default;

  /// Capture the geometry and transforms of a Slug draw for diagnostic presentation.
  /// @param encoded Encoded path geometry for the draw.
  /// @param targetFromPath Transform from path coordinates to target coordinates.
  /// @param rootFromTarget Transform from target coordinates to root presentation coordinates.
  /// @param instanceTransforms Packed per-instance transforms, when instancing is used.
  virtual void recordSlugDraw(const EncodedPath& encoded, const Transform2d& targetFromPath,
                              const Transform2d& rootFromTarget,
                              std::span<const float> instanceTransforms) = 0;
};

/** Frame-wide admission gate invoked before every Slug geometry upload or draw. */
class GeometryAdmission {
public:
  virtual ~GeometryAdmission() = default;

  /** Reserve one or more logical submissions of `encoded` before GPU work is recorded. */
  virtual bool admitGeometry(const EncodedPath& encoded, std::size_t logicalDraws) = 0;

  /** Whether another inline path encode may start in the current frame. */
  [[nodiscard]] virtual bool canEncodeGeometry() const = 0;

  /** Release a successful reservation when submission preparation cannot complete. */
  virtual void releaseGeometry(const EncodedPath& encoded, std::size_t logicalDraws) = 0;

  /**
   * Reserve one instance that draws `encoded` from resident geometry. The frame retains only the
   * instance record, but each instance still evaluates the geometry, so its items still count.
   */
  virtual bool admitResidentInstance(const EncodedPath& encoded) {
    return admitGeometry(encoded, 1u);
  }

  /** Release a reservation made by \ref admitResidentInstance. */
  virtual void releaseResidentInstance(const EncodedPath& encoded) { releaseGeometry(encoded, 1u); }
};

class GeodeBufferPool;
class GeodeDevice;
class GeodeImagePipeline;
class GeodePipeline;
class GeodeGradientPipeline;

/**
 * Parameters for a linear gradient fill at the Geode drawing layer.
 *
 * All geometry fields are in the gradient's own coordinate system (after the
 * caller has already folded `gradientUnits` + the `gradientTransform`
 * attribute into `gradientFromPath`). The encoder uploads these verbatim into
 * a per-draw uniform buffer consumed by `donner/gpu/shader/programs/SlugGradientSource.h`.
 */
struct LinearGradientParams {
  /// Start point in gradient space.
  Vector2d startGrad = Vector2d::Zero();
  /// End point in gradient space.
  Vector2d endGrad = Vector2d::Zero();
  /// Affine transform that maps a path-space sample position to gradient
  /// space. The fragment shader uses this to recover the gradient parameter
  /// `t` per pixel. This is the inverse of `pathFromGradient` and already
  /// bakes in both `gradientUnits` and the `gradientTransform` attribute.
  Transform2d gradientFromPath;
  /// Spread mode at the edges of the gradient range.
  /// 0 = pad, 1 = reflect, 2 = repeat.
  uint32_t spreadMode = 0;
  /// A single gradient stop: normalized offset and STRAIGHT-alpha RGBA.
  /// Not premultiplied: the resolver writes r/g/b/a each divided by 255, and
  /// both gradient shaders interpolate stops in straight alpha and
  /// premultiply only at output (stop-opacity conformance depends on it).
  struct Stop {
    float offset = 0.0f;                       ///< Stop offset in [0, 1].
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};  ///< Straight-alpha RGBA color.
  };
  /// Gradient stops, in order of non-decreasing offset. Colors are in straight alpha, 0..1 per
  /// channel - the shader interpolates in straight alpha and premultiplies at output. Offsets are
  /// in [0, 1]. Up to 16 stops travel inline in the draw's parameters; a gradient with more reads
  /// them from a stop ramp texture, which the draws of one encoder share by stop content and a
  /// resident draw keeps until its stops change. A draw with more than
  /// `GeoEncoder::kMaxGradientRampStops` stops is refused.
  std::span<const Stop> stops;
};

/**
 * Parameters for a radial gradient fill at the Geode drawing layer.
 *
 * Describes a two-circle radial gradient: an outer circle (`center`,
 * `radius`) and a focal circle (`focalCenter`, `focalRadius`). When
 * `focalCenter == center` and `focalRadius == 0`, this reduces to the simple
 * one-circle radial gradient (`t = distance(P, center) / radius`).
 *
 * All geometry fields are in the gradient's own coordinate system, same
 * convention as \ref LinearGradientParams - the caller has already folded
 * `gradientUnits` and `gradientTransform` into `gradientFromPath`.
 */
struct RadialGradientParams {
  /// Outer circle center in gradient space.
  Vector2d center = Vector2d::Zero();
  /// Focal circle center in gradient space. Typically equals `center` unless
  /// the gradient has a distinct focal point (fx/fy).
  Vector2d focalCenter = Vector2d::Zero();
  /// Outer circle radius. Must be > 0.
  double radius = 0.0;
  /// Focal circle radius (SVG 2 `fr` attribute). Typically 0.
  double focalRadius = 0.0;
  /// Same as @ref LinearGradientParams::gradientFromPath.
  Transform2d gradientFromPath;
  /// Spread mode. 0 = pad, 1 = reflect, 2 = repeat.
  uint32_t spreadMode = 0;
  /// Gradient stops. Same wire format as @ref LinearGradientParams -
  /// kept as a sibling struct so each gradient kind's C++ API reflects its
  /// natural parameter set without `std::variant`-style dispatch.
  using Stop = LinearGradientParams::Stop;
  /// Gradient stops; see @ref LinearGradientParams::stops.
  std::span<const Stop> stops;
};

/**
 * The stop ramp one encoder bound for a resolved gradient, so the resolution's further draws in
 * that encoder bind it without hashing or comparing the stops again. The caller keeps one beside
 * each resolution, whose stops must not change while it is kept, and hands it to every draw made
 * from that resolution. It is valid for one encoder only: a draw in another encoder looks the ramp
 * up again and takes the cache over, so a resolution costs one lookup per encoder it draws in.
 */
struct GradientRampCache {
  /// Identity of the encoder that filled the cache; 0 when empty.
  uint64_t encoderId = 0;
  /// The ramp that encoder bound, which its own bookkeeping also keeps alive until submission.
  std::shared_ptr<const GeodeGradientRamp> ramp;
};

/**
 * Drawing API for the Geode GPU renderer.
 *
 * `GeoEncoder` is a per-frame command builder. Construct one against a target
 * texture, issue draw calls (`fillPath`, `clear`, etc.), then call `finish()`
 * to submit the command buffer to the GPU.
 *
 * Per-draw data uses pooled GPU arenas and resident path caches. Small arena uploads are
 * combined in owned staging storage, capped at 64 KiB per arena, and flushed before buffer
 * growth or submission. Rendered pixels remain in GPU textures.
 *
 * Typical usage:
 *
 *     GeoEncoder encoder(device, pipeline, targetTexture);
 *     encoder.clear(css::RGBA::White);
 *     encoder.setTransform(Transform2d::Scale(2.0));
 *     encoder.fillPath(myPath, css::RGBA::Red, FillRule::NonZero);
 *     encoder.finish();
 */
class GeoEncoder {
public:
  /// Most stops a gradient draw may have: one stop ramp's capacity. The renderer's per-frame
  /// gradient-stop budget is no larger, so every draw it admits fits.
  static constexpr std::size_t kMaxGradientRampStops = 256u * 1024u;

  /// Most stops a gradient may have to draw through the cross-entity scene batch, whose paint
  /// block carries them inline; `ensureResidentSceneRecord` refuses a gradient with more.
  static constexpr std::size_t kMaxSceneGradientStops = 16u;

  /**
   * Create an encoder targeting the given texture.
   *
   * @param device The Geode device owning the shared pipelines and counters.
   * @param fillPipeline The Slug fill pipeline.
   * @param gradientPipeline The Slug gradient-fill pipeline.
   * @param imagePipeline The image-blit pipeline.
   * @param target Single-sample render target. Usage must include
   *   `RenderAttachment`; add `TextureBinding` or `CopySrc` when callers
   *   sample or read it after rendering. The texture must outlive `finish()`.
   * @param targetSize Render-target dimensions in physical pixels.
   */
  GeoEncoder(GeodeDevice& device, const GeodePipeline& fillPipeline,
             const GeodeGradientPipeline& gradientPipeline, const GeodeImagePipeline& imagePipeline,
             const gpu::Texture& target, const gpu::Extent2d& targetSize);

  /**
   * Shared-CommandEncoder constructor.
   * Uses the provided `sharedCommandEncoder` instead of creating its own.
   * When this overload is used, `finish()` flushes staged geometry and ends any open render
   * pass - it does NOT finish the CommandEncoder or submit to the
   * queue. The caller (typically `RendererGeode`) owns the encoder's
   * lifetime, finishes it into one of the frame's command buffers, and
   * submits the frame's buffers together.
   *
   * Enables push/pop of isolated layers / filter layers / mask layers
   * without forcing a queue submit per layer boundary - the whole
   * frame's render passes reach the queue as one submission.
   */
  GeoEncoder(GeodeDevice& device, const GeodePipeline& fillPipeline,
             const GeodeGradientPipeline& gradientPipeline, const GeodeImagePipeline& imagePipeline,
             const gpu::Texture& target, const gpu::Extent2d& targetSize,
             gpu::CommandEncoder& sharedCommandEncoder);

  ~GeoEncoder();

  /**
   * Install a cross-frame buffer pool. When set,
   * arena buffer growth prefers recycled buffers from the pool and the
   * destructor returns the fully-grown arena buffers to it instead of
   * destroying them, except after a refused upload. This drives steady-state
   * per-frame `bufferCreates` toward zero without recycling uncertain bytes.
   *
   * Call before the first draw. The pool (typically owned by
   * `RendererGeode::Impl`) must outlive this encoder. Pass `nullptr`
   * to disable (default).
   */
  void setBufferPool(GeodeBufferPool* pool);

  /**
   * Share arena-write failure with the frame owner so it can discard the incomplete frame.
   * @param failed Borrowed per-frame flag; it must outlive this encoder.
   */
  void setArenaWriteFailureFlag(bool* failed);

  /// Refuse the next arena upload without touching the GPU buffer, for error-path tests.
  void injectNextArenaWriteFailureForTesting();

  /** Install the mandatory geometry admission gate for renderer-owned encoders. */
  void setGeometryAdmission(GeometryAdmission* admission);

  /// Number of pattern sampler/view preparation attempts made by this encoder.
  [[nodiscard]] std::size_t patternGpuPreparationsForTesting() const;

  /// Number of admitted scene instances not yet consumed by a batch draw.
  [[nodiscard]] std::size_t pendingSceneAdmissionsForTesting() const;

  /// Fail one scene preparation after `successfulPreparations` successful calls.
  void injectScenePreparationFailureAfterForTesting(std::size_t successfulPreparations);

  /**
   * Record one scene-batch instance's geometry into the debug-overlay sink
   * (no-op without a sink). Ordered batches issue a single GPU draw, so the
   * caller reports each batched instance here in paint order, passing the
   * instance's transform in the same packed 8-float wire format instanced
   * draws use. The encoder transform must already be identity, matching the
   * batch draw's uniform.
   */
  void recordGeometryDebugInstance(const EncodedPath& encoded,
                                   std::span<const float> instanceTransforms);

  /**
   * Observe Slug draws recorded by this encoder.
   *
   * `rootFromTarget` maps this encoder's target pixels to the owning
   * renderer's final target. Pass null to disable observation. The default
   * path stores one pointer and one branch per actual Slug submission.
   */
  /// @param sink Borrowed debug sink; it must outlive its registration until cleared, replaced, or
  /// encoder destruction. Null disables capture.
  /// @param rootFromTarget Maps target-space geometry into the root presentation coordinate system.
  void setGeometryDebugSink(GeometryDebugSink* sink,
                            const Transform2d& rootFromTarget = Transform2d());

  GeoEncoder(const GeoEncoder&) = delete;
  GeoEncoder& operator=(const GeoEncoder&) = delete;
  /// Move constructor.
  GeoEncoder(GeoEncoder&&) noexcept;
  /// Move assignment operator.
  GeoEncoder& operator=(GeoEncoder&&) noexcept;

  /**
   * Clear the target texture to the given color.
   *
   * Must be called before any draw calls - clear is implemented as the load
   * op of the first render pass, so calling it after a draw is a no-op.
   * Subsequent calls override the previous clear color.
   */
  void clear(const css::RGBA& color);

  /**
   * Switch the next render pass's load op from `Clear` to `Load`, preserving
   * whatever the target texture already contains. Useful when the encoder
   * is being reused to append draws on top of previously submitted content
   * (e.g., resuming outer-frame drawing after a nested pattern-tile pass).
   *
   * Must be called before any draws - once a render pass is open it's too
   * late to change its load op.
   */
  void setLoadPreserve();

  /// Enable analytic edge anti-aliasing for subsequent path and mask draws.
  /// Disabled mode emits binary coverage at the pixel center.
  /// @param antialias True to retain analytic edge coverage.
  void setAntialias(bool antialias);

  /// True when a clip polygon or a clip mask is active. Batchable
  /// residence and cross-entity batching are only taken with no active
  /// clip state (the cached bind groups and shared batch uniform carry
  /// zero clip flags).
  bool hasActiveClipState() const;

  /// Monotonic clip-state version: every scissor, clip-polygon, and
  /// clip-mask mutation bumps it. The ordered batch machinery snapshots it
  /// per batch so a batch never spans a clip change.
  uint64_t clipStateVersion() const;

  /// True while a mask pass is open, i.e. draws are being recorded into a
  /// mask texture through the mask pipeline rather than into the main pass.
  /// Residency and the ordered batch gate both exclude these draws: a batch
  /// defers to flush time, by which point the mask pass has closed and the
  /// draw would land in the wrong target.
  bool hasOpenMaskPass() const;

  /// Set the model-view transform for subsequent draw calls.
  void setTransform(const Transform2d& transform);

  /**
   * Set a scissor rectangle in target-pixel coordinates. Subsequent draws
   * are clipped to the intersection of (0,0,targetWidth,targetHeight) and
   * this rectangle. Used by `RendererGeode::pushClip` to implement SVG
   * viewport rect clipping (nested `<svg>` clip, overflow: hidden, etc.).
   *
   * The scissor persists across `setTransform`, `fillPath`, and friends
   * until explicitly cleared via `clearScissorRect`.
   *
   * Negative `x`/`y` and widths/heights that extend past the target are
   * clamped internally; the caller can pass any AABB in pixel space
   * without bounds-checking.
   */
  void setScissorRect(int32_t x, int32_t y, int32_t w, int32_t h);

  /// Remove any active scissor, restoring full-target rasterization.
  void clearScissorRect();

  /**
   * Activate a convex 4-vertex clip polygon.
   *
   * Unlike `setScissorRect`, this clips to the exact parallelogram
   * described by the 4 corners - used for `<symbol>` / `<svg>` /
   * `<use>` viewports that have a non-axis-aligned ancestor transform
   * where WebGPU's rectangular scissor can only express the AABB of
   * the transformed rect, not the true polygon. The fragment shader
   * tests each of 4 edge half-planes at the fragment position and discards
   * fragments outside the polygon.
   *
   * @param corners 4 polygon vertices in target-pixel space, given in
   *   consistent (clockwise OR counter-clockwise) winding order. The
   *   encoder normalises edge normals so a fragment strictly INSIDE
   *   the polygon satisfies every half-plane test.
   */
  void setClipPolygon(const Vector2d corners[4]);

  /// Clear any active clip polygon, restoring unclipped rasterisation
  /// (or falling back to just the scissor rect, if one is set).
  void clearClipPolygon();

  /**
   * Open a new render pass that writes into the given mask
   * texture. Used by `RendererGeode::pushClip` to materialise a
   * path-based clip into an RGBA8Unorm coverage texture that subsequent
   * fill/gradient draws can sample.
   *
   * The main render pass, if open, is closed first. Subsequent
   * `fillPathIntoMask` calls add paths to the mask via the Slug mask
   * pipeline. `endMaskPass` closes the mask pass and re-opens the
   * main pass (with `LoadOp::Load`) when the next draw lands.
   *
   * Mask fills are placed in this encoder's target pixels, and draws
   * sample the mask at those same pixels, so the mask must have the
   * target's extent. A mask of any other extent is refused before
   * anything is recorded. The pass is scissored to the mask's own
   * extent, so it can never exceed its attachment.
   *
   * @param mask Single-sample RGBA8Unorm target with this encoder's
   *   target extent. Sampled by `setClipMask` after `endMaskPass`.
   * @param loadOp Clear for the first pass; Load to union more shapes into an initialized mask.
   * @return Whether the mask pass opened. When it did not, the mask
   *   holds no coverage, and `fillPathIntoMask` and `endMaskPass` do
   *   nothing.
   */
  [[nodiscard]] bool beginMaskPass(const gpu::Texture& mask,
                                   gpu::LoadOp loadOp = gpu::LoadOp::Clear);

  /**
   * Fill `path` into the currently open mask pass using the Slug mask
   * pipeline. Must be called between `beginMaskPass` and `endMaskPass`.
   * The current encoder transform applies (so clip paths use the same
   * device-pixel mapping as the content being clipped).
   */
  /// @param precomputedEncoded Optional cache-hit payload. When
  ///   non-null, the encoder skips `GeodePathEncoder::encode` and the
  ///   `pathEncodes` counter bump. Used by `RendererGeode` to plumb a
  ///   cached `GeodePathCacheComponent::strokeVariants` result.
  /// @param path Path geometry to fill into the open mask pass.
  /// @param rule Fill rule for resolving interior coverage.
  void fillPathIntoMask(const Path& path, FillRule rule,
                        const EncodedPath* precomputedEncoded = nullptr);

  /**
   * Fill `encoded` into the currently open mask pass from `slot`, which keeps the geometry, the
   * mask uniform, and the mask bind group across frames. An unchanged frame writes nothing and
   * creates no bind group: the uniform is rewritten only when its bytes change, and the bind group
   * is rebuilt only when the nested clip mask it binds is a different view.
   *
   * A slot carries one draw's uniform, so a slot an open frame on this device has already drawn
   * (the same clip built twice in one frame, or an outer frame an offscreen pass runs inside)
   * draws this fill through the per-frame arena instead, as does a slot whose upload fails.
   *
   * @param slot Resident slot of this clip shape on this encoder's device. Its slab must be set.
   * @param encoded Cached encode of the clip shape at the current transform's tolerance.
   * @param rule Fill rule the shape was encoded with.
   * @param frameId Device frame generation of the frame recording this fill.
   */
  void fillPathIntoMaskResident(GeodeResidentSlot& slot, const EncodedPath& encoded, FillRule rule,
                                uint64_t frameId);

  /// Close the mask render pass opened by `beginMaskPass`.
  void endMaskPass();

  /**
   * Bind `maskView` as the clip mask texture for subsequent fill /
   * gradient draws. The view must reference a 1-sample RGBA8Unorm
   * texture the same size as the encoder's target - typically the
   * resolve texture produced by `beginMaskPass` + `endMaskPass`.
   *
   * The shader samples `.r` at the pixel center and multiplies it
   * into fragment coverage, so the mask represents a pre-rendered
   * clip region in [0, 1].
   */
  void setClipMask(const gpu::TextureView& maskView);

  /**
   * Preferred overload: sets both the view AND the parent texture so the
   * encoder keeps the underlying Vulkan resource alive for as long as it's
   * bound. The 1-arg overload is kept only for call sites that guarantee
   * the parent's lifetime by other means.
   *
   * See the `activeClipMaskTexture` field comment (and issue #551) for
   * the bug this addresses - without the parent keepalive, popping the
   * clip stack can free a VkImage while the encoder's
   * `createBindGroup` still references its view.
   */
  void setClipMask(const gpu::Texture& maskTexture, const gpu::TextureView& maskView);

  /// Remove any active clip mask, restoring unclipped rasterisation.
  void clearClipMask();

  /**
   * Blit an offscreen texture across the entire target with an alpha
   * multiplier. Used by `RendererGeode::popIsolatedLayer` to composite a
   * sub-layer's content back onto the outer target. The source texture
   * must be the SAME SIZE as this encoder's target - the blit takes the
   * full texture and maps it 1:1 onto the full target. The current
   * `setTransform` does NOT affect the blit (it's a device-pixel-space
   * operation, not a model-space draw).
   *
   * @param src Source texture (RGBA8, premultiplied).
   * @param opacity Overall alpha multiplier in [0, 1].
   * @param retained Uniform buffer and bind group this composite keeps across frames, or null to
   *   allocate both for this frame only. See \ref GeodeTextureEncoder::RetainedQuadBinding for
   *   when a caller may pass one.
   */
  void blitFullTarget(const gpu::Texture& src, double opacity,
                      GeodeTextureEncoder::RetainedQuadBinding* retained = nullptr);

  /**
   * `<mask>` compositing. Same as @ref blitFullTarget but additionally samples a mask texture and
   * derives coverage according to `maskType`. When `maskBounds` is provided, pixels outside the
   * rect are discarded so the `<mask>` element's x/y/width/height are honoured.
   *
   * @param content Offscreen RGBA8 content texture, premultiplied,
   *   same size as this encoder's target.
   * @param mask Offscreen RGBA8 mask texture, premultiplied, same
   *   size as `content`.
   * @param maskType Whether mask coverage comes from luminance or alpha.
   * @param maskBounds Optional clip rect in target-pixel space.
   * @param colorInterpolation Color space for luminance conversion; auto chooses sRGB.
   */
  void blitFullTargetMasked(
      const gpu::Texture& content, const gpu::Texture& mask, svg::MaskType maskType,
      const std::optional<Box2d>& maskBounds,
      svg::ColorInterpolation colorInterpolation = svg::ColorInterpolation::SRGB);

  /**
   * `mix-blend-mode` compositing. Blits `layer` across the
   * entire target using one of the 16 W3C Compositing Level 1 blend
   * formulas, reading the backdrop from `dstSnapshot` (which the
   * caller has already copied from the parent target because the
   * shader cannot read the render pass's own color attachment).
   *
   * Both textures must be the SAME SIZE as this encoder's target
   * and already stored in premultiplied alpha. The encoder's pass
   * must be in a `LoadOp::Clear` state - the blend shader writes the
   * final pixel directly and relies on the render target being
   * zeroed before the draw.
   *
   * @param layer Offscreen RGBA8 layer texture (premultiplied).
   * @param dstSnapshot Frozen copy of the parent target's current
   *   state (premultiplied) - the blend's backdrop.
   * @param blendMode Value in `1..=16` matching the
   *   `donner::svg::MixBlendMode` enumeration. A value of `0`
   *   silently falls through to no-op.
   * @param opacity Group opacity in [0, 1]. Applied to the layer
   *   BEFORE the blend formula so an `opacity` + `mix-blend-mode`
   *   combo on the same element composes correctly - the source
   *   colour that enters the blend is `layer * opacity`.
   * @param retained Uniform buffer and bind group this composite keeps across frames, or null to
   *   allocate both for this frame only. See \ref GeodeTextureEncoder::RetainedQuadBinding for
   *   when a caller may pass one.
   */
  void blitFullTargetBlended(const gpu::Texture& layer, const gpu::Texture& dstSnapshot,
                             uint32_t blendMode, double opacity,
                             GeodeTextureEncoder::RetainedQuadBinding* retained = nullptr);

  /**
   * Draw a raster image into the given destination rectangle.
   *
   * The image's straight-alpha RGBA8 pixels are uploaded to a fresh
   * GPU texture and sampled through the image-blit pipeline. The
   * current transform is applied via the MVP matrix, and the destination
   * rectangle is specified in local (pre-transform) coordinates - i.e.,
   * the transform and the rect compose the same way as any other draw
   * call against this encoder.
   *
   * @param image Decoded RGBA8 image resource. No-op if empty or zero-size.
   * @param destRect Destination rectangle in local (pre-transform) space.
   * @param opacity Overall opacity in [0, 1], combined with the sampled
   *   texel's alpha in the fragment shader.
   * @param imageRendering Resolved smooth, crisp, or two-stage pixelated sampling policy.
   */
  void drawImage(const svg::ImageResource& image, const Box2d& destRect, double opacity,
                 svg::ImageRendering imageRendering);

  /**
   * Draw a raster whose RGBA8 pixels are already premultiplied, such as a decoded color glyph,
   * exactly as \ref drawImage draws a straight-alpha image: the pixels are uploaded to a fresh
   * GPU texture without conversion.
   *
   * @param image Premultiplied RGBA8 image. No-op if empty or zero-size.
   * @param destRect Destination rectangle in local (pre-transform) space.
   * @param opacity Overall opacity in [0, 1].
   * @param imageRendering Resolved smooth, crisp, or two-stage pixelated sampling policy.
   */
  void drawPremultipliedImage(const svg::ImageResource& image, const Box2d& destRect,
                              double opacity, svg::ImageRendering imageRendering);

  /**
   * Whether \ref drawImage would draw \p image into \p destRect: the payload is a non-empty,
   * exactly sized RGBA8 buffer within this device's texture size limit, and the destination is
   * not empty.
   *
   * @param image Decoded RGBA8 image resource.
   * @param destRect Destination rectangle in local (pre-transform) space.
   */
  [[nodiscard]] bool acceptsImage(const svg::ImageResource& image, const Box2d& destRect) const;

  /**
   * Upload \p image's straight-alpha pixels, premultiplied, to a new sampled texture that
   * \ref drawImageTexture can draw. The texture is not retained by this encoder.
   *
   * @param image Decoded RGBA8 image that \ref acceptsImage accepts.
   * @return The texture, or an empty one when the device refused it.
   */
  [[nodiscard]] gpu::Texture uploadImageTexture(const svg::ImageResource& image);

  /**
   * Draw a premultiplied texture uploaded by \ref uploadImageTexture exactly as \ref drawImage
   * draws the image it was uploaded from.
   *
   * @param texture Premultiplied RGBA8 texture holding the image's pixels.
   * @param imageSize Width and height of the image in pixels.
   * @param destRect Destination rectangle in local (pre-transform) space.
   * @param opacity Overall opacity in [0, 1].
   * @param imageRendering Resolved smooth, crisp, or two-stage pixelated sampling policy.
   * @param retained Uniform buffer and bind group this draw keeps across frames, or null to
   *   allocate both for this frame only. See \ref GeodeTextureEncoder::RetainedQuadBinding for
   *   when a caller may pass one.
   */
  void drawImageTexture(const gpu::Texture& texture, const Vector2i& imageSize,
                        const Box2d& destRect, double opacity, svg::ImageRendering imageRendering,
                        GeodeTextureEncoder::RetainedQuadBinding* retained = nullptr);

  /// Source-compatible legacy overload. New callers pass the resolved image-rendering enum.
  void drawImage(const svg::ImageResource& image, const Box2d& destRect, double opacity,
                 bool pixelated) {
    drawImage(image, destRect, opacity,
              pixelated ? svg::ImageRendering::Pixelated : svg::ImageRendering::Auto);
  }

  /**
   * Draw an already-GPU-resident RGBA texture into the destination rectangle.
   *
   * The current transform is applied exactly like \ref drawImage, but the
   * texture is sampled directly without a CPU upload.
   *
   * @param texture GPU-resident RGBA texture.
   * @param destRect Destination rectangle in local (pre-transform) space.
   * @param sourceUv Source UV rectangle in [0,1] x [0,1]. Producers that keep an oversized
   *   backing allocation pass the sub-rect that holds the valid content.
   * @param opacity Overall opacity in [0, 1].
   * @param pixelated If true, use nearest-neighbor filtering. Otherwise, bilinear.
   * @param sourceIsPremultiplied True when the texels are premultiplied, as Geode
   *   render-target snapshots are. CPU bitmaps uploaded by host presentation code are
   *   straight-alpha and must pass false.
   * @param retained Uniform buffer and bind group this draw keeps across frames, or null to
   *   allocate both for this frame only. See \ref GeodeTextureEncoder::RetainedQuadBinding for
   *   when a caller may pass one.
   */
  void drawTexture(const gpu::Texture& texture, const Box2d& destRect, const Box2d& sourceUv,
                   double opacity, bool pixelated, bool sourceIsPremultiplied,
                   GeodeTextureEncoder::RetainedQuadBinding* retained = nullptr);

  /**
   * Draw a GPU-resident texture like \ref drawTexture with bilinear filtering, mapping each target
   * pixel to its source texel relative to \p anchor. Two draws whose source and target differ
   * only by whole pixels, with the anchor at the same target pixel relative to the content,
   * produce identical pixels; interpolated texture coordinates and hardware filtering do not
   * guarantee that when the mapping is rotated, skewed or fractionally scaled.
   *
   * @param texture GPU-resident RGBA texture.
   * @param destRect Destination rectangle in local (pre-transform) space.
   * @param sourceUv Source UV rectangle in [0,1] x [0,1]; samples clamp to it.
   * @param opacity Overall opacity in [0, 1].
   * @param sourceIsPremultiplied True when the texels are premultiplied.
   * @param anchor Target pixel the sampled content is aligned to.
   */
  void drawTextureAnchored(const gpu::Texture& texture, const Box2d& destRect,
                           const Box2d& sourceUv, double opacity, bool sourceIsPremultiplied,
                           const Vector2i& anchor);

  /**
   * Fill a path with a solid color.
   *
   * The path is encoded into Slug band data on the CPU, uploaded to GPU
   * buffers, and a draw call is recorded. The fill is applied with the
   * current transform.
   *
   * @param path The path to fill.
   * @param color Solid fill color (NOT premultiplied - the encoder handles
   *   premultiplication for the blend pipeline).
   * @param rule Fill rule (NonZero or EvenOdd).
   */
  /// @param precomputedEncoded Optional cache-hit payload - see
  ///   `GeodePathCacheComponent`. When non-null, skips encode +
  ///   counter bump.
  void fillPath(const Path& path, const css::RGBA& color, FillRule rule,
                const EncodedPath* precomputedEncoded = nullptr);

  /**
   * Solid fill of a cached path with persistent GPU residence.
   *
   * `slot` is a per-entity `GeodeResidentSlot` (owned by
   * `GeodeResidentPathComponent`, invalidated by the same
   * `ComputedPathComponent` listener that clears the CPU encode cache). On
   * first use the encoder uploads `encoded` into a persistent combined
   * Vertex|Storage|Uniform buffer and builds a cached bind group; on every
   * subsequent unchanged frame it re-uses both and writes zero geometry
   * bytes. Only the 288-byte per-draw uniform is rewritten, and only when
   * it actually changed (camera/color move), so a static document's
   * steady-state frame writes ~zero bytes and creates zero bind groups.
   *
   * A draw under an active path-clip mask stays resident: its uniform
   * carries the clip flag exactly as the arena draw's does, and the cached
   * bind group binds the mask's view, rebuilt only when the draw is clipped
   * by a different view. Under a clip polygon or an open mask pass the call
   * falls back to the per-frame arena path (a fresh per-draw bind group).
   *
   * @param slot Persistent residence for this entity's fill or stroke
   *   encode. Must outlive the frame's submit (it does: the ECS component
   *   is only removed on geometry change, after the prior frame submitted).
   * @param encoded Cached encode (stable across frames). Must be non-empty
   *   for a draw to land.
   * @param color Solid fill color (NOT premultiplied - premultiplied here,
   *   identically to `fillPath`).
   * @param rule Fill rule.
   * @param frameId Monotonic frame index. A slot serves at most one
   *   resident draw per frame (its single uniform buffer cannot hold
   *   distinct per-instance uniforms); repeat draws in the same frame fall
   *   back to the arena path.
   */
  void fillPathResident(GeodeResidentSlot& slot, const EncodedPath& encoded, const css::RGBA& color,
                        FillRule rule, uint64_t frameId);

  /**
   * Fill N copies of the same encoded path at N different affine
   * transforms, in one GPU draw call.
   *
   * The caller has already packed each transform into the wire format
   * the shader expects: two `vec4f` rows per instance (row-major affine,
   * 32 bytes per entry). See `donner/gpu/shader/programs/SlugFillSource.h`
   * `struct InstanceTransform` for the exact layout.
   *
   * The vertex shader composes each instance's transform with the
   * currently-bound `uniforms.mvp`, so `encoder.setTransform(...)`
   * still applies an outer world transform; per-instance data is the
   * *delta* relative to that. Pass `Transform2d::Identity` to
   * `setTransform` if instance transforms already encode the full
   * path→clip-space mapping (the common case for `<use>` batching,
   * where each instance's transform is the full `worldFromEntity`).
   *
   * If `instanceTransforms` is empty the call is a no-op. If
   * `instanceTransforms.size() == 1` this is equivalent to a single
   * `fillPath` with the given transform folded in - the caller can
   * still prefer it when bouncing through the batcher.
   *
   * @param encoded Precomputed `EncodedPath` shared across all
   *   instances. Required (non-null) - there's no "encode inline"
   *   path here; the whole point is to amortize one encode across
   *   many draws.
   * @param color Solid fill color (NOT premultiplied).
   * @param rule Fill rule.
   * @param instanceTransforms Span of `{row0, row1}` vec4f pairs,
   *   two consecutive `vec4f` per instance (32 bytes each). The span
   *   must contain exactly `8 * instanceCount` floats.
   */
  void fillPathInstanced(const EncodedPath& encoded, const css::RGBA& color, FillRule rule,
                         std::span<const float> instanceTransforms);

  /**
   * Paint carried by one cross-entity batch instance: a solid colour, or a
   * resolved gradient whose parameters and stops the encoder copies into
   * the slot's persistent paint block. The block holds 16 stops, so
   * `ensureResidentSceneRecord` declines a gradient with more and the caller
   * draws it through the gradient fill calls. The gradient pointers are
   * borrowed for the duration of the call only.
   */
  struct ScenePaint {
    css::RGBA color = css::RGBA(
        0, 0, 0, 255);  //!< Unpremultiplied solid fill color when no gradient is selected.
    const LinearGradientParams* linearGradient =
        nullptr;  //!< Borrowed linear-gradient parameters for the duration of the call.
    const RadialGradientParams* radialGradient =
        nullptr;  //!< Borrowed radial-gradient parameters for the duration of the call.

    /// True when this instance paints with a gradient rather than a colour.
    bool isGradient() const { return linearGradient != nullptr || radialGradient != nullptr; }
  };

  /**
   * The record fields a batched instance takes from encoder-side state rather
   * than from its own arguments: the paint scalars published into its slot and
   * the clip rectangle live at the time it was appended.
   *
   * A batch re-derives every instance's record when it flushes, which can be
   * arbitrarily far from the append: other draws of the same entity, and other
   * clip changes, can happen in between. Reading these back off the slot or off
   * the encoder at that point reads the LATEST state, not the appended one. The
   * caller therefore captures them at append and hands the same values back at
   * flush, which is what makes the re-derivation reproduce byte-identical
   * bytes.
   */
  struct SceneRecordState {
    uint32_t paintMode = 0;
    uint32_t gradientSpread = 0;
    uint32_t gradientStopCount = 0;
    uint32_t clipRectActive = 0;
    float clipRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  };

  /**
   * Ensure a resident slot's geometry is uploaded and its instance record
   * (record-slab slot, chunk-relative bases, transform-bearing) is current
   * WITHOUT drawing. Used by the cross-entity ordered-batch path, which
   * draws many slots in one GPU call; solo draws call `fillPathResident`
   * instead, which performs the same work and then records the draw.
   *
   * @param slot Resident slot (geometry + record slab wiring installed by
   *   the renderer).
   * @param encoded Precomputed `EncodedPath` shared with the cache.
   * @param paint Solid color or borrowed gradient parameters for this instance.
   * @param rule Fill rule.
   * @param recordTransform The full deviceFromLocal transform to bake
   *   into the record (the batch uniform is orthographic-only).
   * @param recordSlotOverride When non-null, write the record into this
   *   slot instead of the entity's primary slot (same-frame repeat draws
   *   need one record per draw, and per-occurrence text records live outside
   *   the shared glyph slot entirely).
   * @param overrideRecordCache Optional caller-owned copy of the override
   *   slot's last contents. Supplying it turns the override write into the
   *   same skip-when-unchanged write the primary record gets, which is what
   *   lets a persistent per-occurrence record reach a zero-write steady
   *   state. Null means the override slot is per-frame scratch and is always
   *   written. Ignored when `recordSlotOverride` is null.
   * @param recordState Carries the record fields that come from encoder-side
   *   state. When `publishPaint` is true this is an OUT parameter: `paint` is
   *   published into the slot and the resulting scalars, plus the live clip
   *   rectangle, are written here. When it is false this is an IN parameter:
   *   the record takes these exact values and neither the slot's paint nor the
   *   encoder's current clip is consulted. Null on either side falls back to
   *   the slot's and the encoder's current state.
   * @param publishPaint True to publish `paint` into the slot's paint block
   *   and paint scalars; false to replay `recordState` instead. A batch's
   *   flush-time re-ensure passes false, because by then the slot's paint may
   *   belong to a later draw and the encoder's clip may have moved on.
   * @return True when the slot is resident and current.
   */
  bool ensureResidentSceneRecord(GeodeResidentSlot& slot, const EncodedPath& encoded,
                                 const ScenePaint& paint, FillRule rule,
                                 const Transform2d& recordTransform,
                                 const GeodeRecordSlab::Slot* recordSlotOverride = nullptr,
                                 std::vector<uint8_t>* overrideRecordCache = nullptr,
                                 SceneRecordState* recordState = nullptr, bool publishPaint = true);

  /** Cancel one prepared scene instance before demoting it to a different draw path. */
  void releasePreparedSceneAdmission(const EncodedPath& encoded);

  /// One cross-entity ordered batch: consecutive resident slots of one
  /// slab chunk plus a run of consecutive record-slab slots.
  struct SceneBatchBinding {
    gpu::BufferRef chunkBuffer;   ///< Slab chunk holding every instance's geometry.
    gpu::BufferRef recordBuffer;  ///< Record-slab buffer holding the records.

    /// Stable identities of `chunkBuffer` / `recordBuffer` (see
    /// `GeodeDevice::AllocateBufferId`). The bind-group cache outlives the
    /// document that owns these buffers, so it keys on these ids; the raw
    /// handle addresses are recycled once that document is destroyed.
    uint64_t chunkBufferId = 0;
    /// Byte size of `chunkBuffer`; scene batches bind the whole chunk.
    uint64_t chunkBytes = 0;
    uint64_t recordBufferId = 0;
    /// BYTE offset of the first instance's record inside recordBuffer.
    /// Record-slot indices are global across slab chunks, so an index is
    /// not a byte offset once the slab has grown; the caller passes the
    /// first slot's own buffer-relative offset instead.
    uint64_t firstRecordOffset = 0;
    uint32_t instanceCount = 1;
    uint32_t vertexCount = 0;  ///< Max fan vertex count over the instances.

    /// Record slab the instances' records live in. Supplies the persistent
    /// batch-uniform buffer so a steady frame writes nothing; null falls
    /// back to the encoder's per-frame uniform arena.
    GeodeRecordSlab* recordSlab = nullptr;
  };

  /**
   * Record ONE GPU draw covering a cross-entity batch of resident solid
   * fills (ordered batching). The caller has already ensured each
   * instance's record via `ensureResidentSceneRecord`. The bind group
   * binds the whole slab chunk for every geometry class (the records carry
   * chunk-relative element bases) and the record span for binding 7. The
   * draw issues `firstInstance = binding.firstInstance` and the maximum
   * fan vertex count; the vertex shader folds extra triangles of smaller
   * instances into a degenerate position.
   *
   * The encoder transform must be identity when this is called (the batch
   * uniform carries the orthographic mapping only).
   *
   * @param color Solid fill color shared by every instance.
   * @param rule Fill rule shared by every instance.
   * @param binding Batch binding (chunk + record span + counts).
   */
  void fillPathSceneBatch(const css::RGBA& color, FillRule rule, const SceneBatchBinding& binding);

  /**
   * Fill a path with a linear gradient.
   *
   * Same CPU-side Slug band encoding as @ref fillPath, but the shading stage
   * uses the linear-gradient pipeline - each pixel inside the path is colored
   * by sampling the stop list at a per-pixel parameter `t` computed from
   * `params.gradientFromPath`, `params.startGrad`, and `params.endGrad`, then
   * folded through `params.spreadMode`.
   *
   * If the path encodes to zero bands or the stop list is empty, the call is
   * a no-op.
   *
   * @param path The path to fill.
   * @param params Linear gradient parameters. Colors are straight alpha in
   *   0..1 per channel; the encoder premultiplies before upload.
   * @param rule Fill rule (NonZero or EvenOdd).
   * @param precomputedEncoded Optional cached path encoding; null encodes the path for this draw.
   * @param rampCache Optional stop ramp cache of the resolution \p params come from; see
   *   @ref GradientRampCache.
   */
  void fillPathLinearGradient(const Path& path, const LinearGradientParams& params, FillRule rule,
                              const EncodedPath* precomputedEncoded = nullptr,
                              GradientRampCache* rampCache = nullptr);

  /**
   * Fill a path with a radial gradient.
   *
   * Same CPU encoding and GPU-dispatch machinery as @ref
   * fillPathLinearGradient, but the gradient parameter `t` is derived from
   * a two-circle radial construction in the shader (see
   * `radial_t()` in `donner/gpu/shader/programs/SlugGradientSource.h`).
   *
   * If the path encodes to zero bands, the stop list is empty, or
   * `params.radius <= 0`, the call is a no-op.
   *
   * @param path The path to fill.
   * @param params Radial gradient parameters (center + radius, optional
   *   focal point + radius, shared transform and stops).
   * @param rule Fill rule (NonZero or EvenOdd).
   * @param precomputedEncoded Optional cached path encoding; null encodes the path for this draw.
   * @param rampCache Optional stop ramp cache of the resolution \p params come from; see
   *   @ref GradientRampCache.
   */
  void fillPathRadialGradient(const Path& path, const RadialGradientParams& params, FillRule rule,
                              const EncodedPath* precomputedEncoded = nullptr,
                              GradientRampCache* rampCache = nullptr);

  /**
   * Fill a path with a linear gradient through a persistent GPU residence
   * slot.
   *
   * The slot's combined buffer holds the encoded band/curve/grid data and
   * the 704-byte gradient uniform block; on an unchanged frame the draw
   * rewrites nothing and reuses the cached 11-binding bind group, which
   * binds the active path-clip mask's view (or the identity dummy) and is
   * rebuilt when the draw is clipped by a different view. Falls back to the
   * per-frame arena path internally when a clip polygon or mask pass is
   * active.
   *
   * @param slot Resident gradient slot for this entity's fill.
   * @param encoded Precomputed encode from the cached-path encode cache.
   * @param params Resolved linear gradient parameters.
   * @param rule Fill rule (NonZero or EvenOdd).
   * @param frameId Current frame index (drives the once-per-frame gate).
   */
  void fillPathLinearGradientResident(GeodeResidentGradientSlot& slot, const EncodedPath& encoded,
                                      const LinearGradientParams& params, FillRule rule,
                                      uint64_t frameId);

  /**
   * Radial variant of @ref fillPathLinearGradientResident. Same residence
   * contract and arena fallback.
   */
  void fillPathRadialGradientResident(GeodeResidentGradientSlot& slot, const EncodedPath& encoded,
                                      const RadialGradientParams& params, FillRule rule,
                                      uint64_t frameId);

  /**
   * Describes a pattern tile used as a paint source for `fillPathPattern`.
   *
   * The tile texture is expected to contain pre-rendered pattern content in
   * premultiplied RGBA. The Slug fill shader samples it with the `Repeat`
   * wrap mode (equivalent to SVG `<pattern>` default behaviour) using the
   * provided transform to map path-space positions into tile-space.
   */
  struct PatternPaint {
    /// Pre-rendered tile texture (RGBA8, premultiplied).
    const gpu::Texture* tile = nullptr;
    /// View of \ref tile that outlives the frame, for @ref fillPathPatternResident, whose cached
    /// bind group names it. Null when the caller has none; the arena fill creates its own.
    gpu::TextureViewRef tileView;
    /// Size of the tile rectangle in pattern space (width, height). The
    /// shader uses this to wrap sample positions via `fract()`.
    Vector2d tileSize;
    /// Transform from path space (where the path being filled lives) to
    /// pattern tile space. Typically `inverse(targetFromPattern)` composed
    /// with the current encoder transform - the RendererGeode layer builds
    /// the right composition.
    Transform2d patternFromPath;
    /// Multiplicative alpha applied to the sampled tile color. Usually
    /// `fill-opacity * opacity`.
    double opacity = 1.0;
  };

  /**
   * Fill a path with a repeating pattern tile. The pattern texture must have
   * been rendered earlier (e.g., by a nested `GeoEncoder`) and contains
   * premultiplied RGBA. The shader applies the Slug winding-number coverage
   * test identically to `fillPath`, and samples the tile for pixels inside
   * the path.
   */
  void fillPathPattern(const Path& path, FillRule rule, const PatternPaint& paint,
                       const EncodedPath* precomputedEncoded = nullptr);

  /**
   * Pattern fill of a cached path through a persistent GPU residence slot, on the same terms as
   * @ref fillPathResident: the geometry is uploaded once, the slot's uniform is rewritten only
   * when its bytes change, and the cached bind group is rebuilt only when the bound tile view
   * changes. Pixels match @ref fillPathPattern. Falls back to the per-frame arena path under an
   * active clip polygon or mask pass, or for a second draw of the slot in one frame; under a
   * path-clip mask it stays resident like @ref fillPathResident.
   *
   * @param slot Persistent residence for this entity's fill encode.
   * @param encoded Cached encode (stable across frames).
   * @param rule Fill rule.
   * @param paint Pattern tile to sample; draws nothing without a tile and a \ref
   *   PatternPaint::tileView.
   * @param frameId Monotonic frame index (drives the once-per-frame gate).
   */
  void fillPathPatternResident(GeodeResidentSlot& slot, const EncodedPath& encoded, FillRule rule,
                               const PatternPaint& paint, uint64_t frameId);

  /**
   * Submit all encoded commands to the GPU queue.
   *
   * After this call, the encoder is in a "finished" state and no further
   * draws can be issued. The caller is responsible for any synchronization
   * (e.g., MapAsync + Tick loop) needed to actually use the rendered output.
   */
  void finish();

private:
  struct Impl;
  struct FillDrawArgs;

  /// Shared path for \ref drawTexture and \ref drawTextureAnchored.
  void drawTextureWithFilter(const gpu::Texture& texture, const Box2d& destRect,
                             const Box2d& sourceUv, double opacity,
                             GeodeTextureEncoder::Filter filter, bool sourceIsPremultiplied,
                             const Vector2i& anchor,
                             GeodeTextureEncoder::RetainedQuadBinding* retained = nullptr);

  /// Shared path for solid and pattern fills: encode path, upload buffers,
  /// build bind group, record draw call.
  void submitFillDraw(const FillDrawArgs& args, std::span<const float> instanceTransforms = {},
                      bool requireAdmission = true);

  /// The draw arguments of a pattern fill, built in one place for the arena and the resident
  /// pattern fills so the two paint the same tile with the same mapping and opacity.
  /// @param path Path to encode when \p encoded is not used, or null.
  /// @param encoded The fill's encode.
  /// @param rule Fill rule.
  /// @param paint Pattern tile, mapping and opacity.
  /// @param tileView View of the tile to bind.
  /// @param sampler Linear repeat sampler to bind.
  static FillDrawArgs PatternFillArgs(const Path* path, const EncodedPath& encoded, FillRule rule,
                                      const PatternPaint& paint, gpu::TextureViewRef tileView,
                                      gpu::SamplerRef sampler);

  /// Shared path for the solid and pattern resident fills: draws through \p slot when residence
  /// is allowed and falls back to \ref submitFillDraw otherwise.
  void submitResidentOrArenaFill(GeodeResidentSlot& slot, const EncodedPath& encoded,
                                 const FillDrawArgs& args, uint64_t frameId);

  /// Shared construction helpers used by both constructors. Factored out
  /// to avoid duplicating ~20 lines of setup. See GeoEncoder.cc.
  static void initImpl(Impl& impl, GeodeDevice& device, const GeodePipeline& fillPipeline,
                       const GeodeGradientPipeline& gradientPipeline,
                       const GeodeImagePipeline& imagePipeline, const gpu::Texture& target,
                       const gpu::Extent2d& targetSize);
  static void finalizeImpl(Impl& impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::geode
