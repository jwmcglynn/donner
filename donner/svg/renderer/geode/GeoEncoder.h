#pragma once
/// @file
/// Drawing API for the Geode GPU renderer.

#include <webgpu/webgpu.hpp>

#include <cstdint>
#include <memory>
#include <span>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"

namespace donner {
class Path;
}  // namespace donner

namespace donner::svg {
struct ImageResource;
}  // namespace donner::svg

namespace donner::geode {

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
 * a per-draw uniform buffer consumed by `shaders/slug_gradient.wgsl`.
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
  /// Gradient stops. Colors are in straight alpha, 0..1 per channel — the
  /// encoder premultiplies before upload. Offsets must be in [0, 1].
  ///
  /// A hard cap of 16 stops is enforced inside the encoder to match the
  /// fixed-size uniform buffer layout in `slug_gradient.wgsl`. Stops beyond
  /// the cap are silently truncated — a follow-up will move stop storage to
  /// a texture lookup (`GeodeGradientCacheComponent`) to lift this limit.
  /// A single gradient stop: normalized offset and premultiplied linear RGBA.
  struct Stop {
    float offset = 0.0f;  ///< Stop offset in [0, 1].
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};  ///< Premultiplied linear RGBA color.
  };
  /// Gradient stops (capped at 16 entries by the underlying uniform buffer).
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
 * convention as \ref LinearGradientParams — the caller has already folded
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
  /// Gradient stops. Same wire format / cap as @ref LinearGradientParams —
  /// kept as a sibling struct so each gradient kind's C++ API reflects its
  /// natural parameter set without `std::variant`-style dispatch.
  using Stop = LinearGradientParams::Stop;
  /// Gradient stops (capped at 16 entries by the underlying uniform buffer).
  std::span<const Stop> stops;
};

/**
 * Drawing API for the Geode GPU renderer.
 *
 * `GeoEncoder` is a per-frame command builder. Construct one against a target
 * texture, issue draw calls (`fillPath`, `clear`, etc.), then call `finish()`
 * to submit the command buffer to the GPU.
 *
 * The encoder owns no GPU buffers itself — each draw call allocates fresh
 * vertex / band / curve / uniform buffers. This is the simplest possible
 * implementation; later phases will add buffer pooling and the ECS-backed
 * `GeodePathCacheComponent` for paths whose geometry hasn't changed.
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
  /**
   * Create an encoder targeting the given texture pair.
   *
   * The encoder uses 4× MSAA internally: every render pass attaches
   * `msaaTarget` as the color attachment and `resolveTarget` as the
   * pass's resolve attachment. The MSAA attachment's store op is `Store`
   * so multi-pass work (e.g., re-opening a pass via `setLoadPreserve()`
   * after a nested layer composite) can pick up the previous MSAA state
   * via `LoadOp::Load`.
   *
   * External code reads back / samples from the `resolveTarget` (always
   * 1-sample), never from the MSAA texture, because sampling an MSAA
   * texture requires `texture_multisampled_2d` bindings in WGSL which the
   * image-blit shader doesn't use.
   *
   * @param device The Geode device (owns the wgpu::Device + queue).
   * @param fillPipeline The Slug fill pipeline (4× multisample).
   * @param gradientPipeline The Slug gradient-fill pipeline (4× multisample).
   * @param imagePipeline The image-blit pipeline (4× multisample).
   * @param msaaTarget 4× multisampled render target texture. Usage must
   *   include `RenderAttachment`. Same width/height as `resolveTarget`.
   * @param resolveTarget 1-sample resolve texture. Usage must include
   *   `RenderAttachment` + `TextureBinding` + `CopySrc` (for readback).
   *   The encoder retains references to both; both must outlive
   *   `finish()`.
   */
  GeoEncoder(GeodeDevice& device, const GeodePipeline& fillPipeline,
             const GeodeGradientPipeline& gradientPipeline,
             const GeodeImagePipeline& imagePipeline, const wgpu::Texture& msaaTarget,
             const wgpu::Texture& resolveTarget);

  ~GeoEncoder();

  GeoEncoder(const GeoEncoder&) = delete;
  GeoEncoder& operator=(const GeoEncoder&) = delete;
  /// Move constructor.
  GeoEncoder(GeoEncoder&&) noexcept;
  /// Move assignment operator.
  GeoEncoder& operator=(GeoEncoder&&) noexcept;

  /**
   * Clear the target texture to the given color.
   *
   * Must be called before any draw calls — clear is implemented as the load
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
   * Must be called before any draws — once a render pass is open it's too
   * late to change its load op.
   */
  void setLoadPreserve();

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
   * Activate a convex 4-vertex clip polygon (Phase 3a).
   *
   * Unlike `setScissorRect`, this clips to the exact parallelogram
   * described by the 4 corners — used for `<symbol>` / `<svg>` /
   * `<use>` viewports that have a non-axis-aligned ancestor transform
   * where WebGPU's rectangular scissor can only express the AABB of
   * the transformed rect, not the true polygon. The fragment shader
   * tests each of 4 edge half-planes against its sub-pixel sample
   * positions and AND's the result into `@builtin(sample_mask)` so
   * clipping integrates with the 4× MSAA coverage path.
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
   * Phase 3b: open a new render pass that writes into the given mask
   * texture pair. Used by `RendererGeode::pushClip` to materialise a
   * path-based clip into an R8Unorm coverage texture that subsequent
   * fill/gradient draws can sample.
   *
   * The main render pass, if open, is closed first. Subsequent
   * `fillPathIntoMask` calls add paths to the mask via the Slug mask
   * pipeline. `endMaskPass` closes the mask pass and re-opens the
   * main pass (with `LoadOp::Load`) when the next draw lands.
   *
   * @param msaaMask 4× MSAA R8Unorm render target. Must be the same
   *   size as this encoder's target. Cleared to 0 at the start of
   *   the pass.
   * @param resolveMask 1-sample R8Unorm resolve target. Sampled by
   *   `setClipMask` after `endMaskPass`.
   */
  void beginMaskPass(const wgpu::Texture& msaaMask, const wgpu::Texture& resolveMask);

  /**
   * Fill `path` into the currently open mask pass using the Slug mask
   * pipeline. Must be called between `beginMaskPass` and `endMaskPass`.
   * The current encoder transform applies (so clip paths use the same
   * device-pixel mapping as the content being clipped).
   */
  void fillPathIntoMask(const Path& path, FillRule rule);

  /// Close the mask render pass opened by `beginMaskPass`.
  void endMaskPass();

  /**
   * Bind `maskView` as the clip mask texture for subsequent fill /
   * gradient draws. The view must reference a 1-sample R8Unorm
   * texture the same size as the encoder's target — typically the
   * resolve texture produced by `beginMaskPass` + `endMaskPass`.
   *
   * The shader samples `.r` at the pixel center and multiplies it
   * into fragment coverage, so the mask represents a pre-rendered
   * clip region in [0, 1].
   */
  void setClipMask(const wgpu::TextureView& maskView);

  /// Remove any active clip mask, restoring unclipped rasterisation.
  void clearClipMask();

  /**
   * Blit an offscreen texture across the entire target with an alpha
   * multiplier. Used by `RendererGeode::popIsolatedLayer` to composite a
   * sub-layer's content back onto the outer target. The source texture
   * must be the SAME SIZE as this encoder's target — the blit takes the
   * full texture and maps it 1:1 onto the full target. The current
   * `setTransform` does NOT affect the blit (it's a device-pixel-space
   * operation, not a model-space draw).
   *
   * @param src Source texture (RGBA8, premultiplied).
   * @param opacity Overall alpha multiplier in [0, 1].
   */
  void blitFullTarget(const wgpu::Texture& src, double opacity);

  /**
   * Phase 3c `<mask>` compositing. Same as @ref blitFullTarget but
   * additionally samples a luminance mask texture and multiplies the
   * content by the mask's BT.709 luminance (× alpha, matching
   * tiny-skia's `Mask::fromPixmap(Luminance)`). When `maskBounds` is
   * provided, pixels outside the rect are discarded so the `<mask>`
   * element's x/y/width/height are honoured.
   *
   * @param content Offscreen RGBA8 content texture, premultiplied,
   *   same size as this encoder's target.
   * @param mask Offscreen RGBA8 mask texture, premultiplied, same
   *   size as `content`. The mask's luminance × alpha becomes a
   *   per-pixel coverage multiplier on the content.
   * @param maskBounds Optional clip rect in target-pixel space.
   */
  void blitFullTargetMasked(const wgpu::Texture& content, const wgpu::Texture& mask,
                            const std::optional<Box2d>& maskBounds);

  /**
   * Draw a raster image into the given destination rectangle.
   *
   * The image's straight-alpha RGBA8 pixels are uploaded to a fresh
   * `wgpu::Texture` and sampled through the image-blit pipeline. The
   * current transform is applied via the MVP matrix, and the destination
   * rectangle is specified in local (pre-transform) coordinates — i.e.,
   * the transform and the rect compose the same way as any other draw
   * call against this encoder.
   *
   * @param image Decoded RGBA8 image resource. No-op if empty or zero-size.
   * @param destRect Destination rectangle in local (pre-transform) space.
   * @param opacity Overall opacity in [0, 1], combined with the sampled
   *   texel's alpha in the fragment shader.
   * @param pixelated If true, use nearest-neighbor filtering (for
   *   `image-rendering: pixelated`). Otherwise, bilinear.
   */
  void drawImage(const svg::ImageResource& image, const Box2d& destRect, double opacity,
                 bool pixelated);

  /**
   * Fill a path with a solid color.
   *
   * The path is encoded into Slug band data on the CPU, uploaded to GPU
   * buffers, and a draw call is recorded. The fill is applied with the
   * current transform.
   *
   * @param path The path to fill.
   * @param color Solid fill color (NOT premultiplied — the encoder handles
   *   premultiplication for the blend pipeline).
   * @param rule Fill rule (NonZero or EvenOdd).
   */
  void fillPath(const Path& path, const css::RGBA& color, FillRule rule);

  /**
   * Fill a path with a linear gradient.
   *
   * Same CPU-side Slug band encoding as @ref fillPath, but the shading stage
   * uses the linear-gradient pipeline — each pixel inside the path is colored
   * by sampling the stop list at a per-pixel parameter `t` computed from
   * `params.gradientFromPath`, `params.startGrad`, and `params.endGrad`, then
   * folded through `params.spreadMode`.
   *
   * If the path encodes to zero bands or the stop list is empty, the call is
   * a no-op.
   *
   * @param path The path to fill.
   * @param params Linear gradient parameters. Colors are straight alpha in
   *   0..1 per channel; the encoder premultiplies before upload. Up to 16
   *   stops are honored; excess are silently truncated with a one-shot
   *   verbose warning at the call site in `RendererGeode`.
   * @param rule Fill rule (NonZero or EvenOdd).
   */
  void fillPathLinearGradient(const Path& path, const LinearGradientParams& params, FillRule rule);

  /**
   * Fill a path with a radial gradient.
   *
   * Same CPU encoding and GPU-dispatch machinery as @ref
   * fillPathLinearGradient, but the gradient parameter `t` is derived from
   * a two-circle radial construction in the shader (see
   * `radial_t()` in `shaders/slug_gradient.wgsl`).
   *
   * If the path encodes to zero bands, the stop list is empty, or
   * `params.radius <= 0`, the call is a no-op.
   *
   * @param path The path to fill.
   * @param params Radial gradient parameters (center + radius, optional
   *   focal point + radius, shared transform and stops).
   * @param rule Fill rule (NonZero or EvenOdd).
   */
  void fillPathRadialGradient(const Path& path, const RadialGradientParams& params, FillRule rule);

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
    wgpu::Texture tile;
    /// Size of the tile rectangle in pattern space (width, height). The
    /// shader uses this to wrap sample positions via `fract()`.
    Vector2d tileSize;
    /// Transform from path space (where the path being filled lives) to
    /// pattern tile space. Typically `inverse(targetFromPattern)` composed
    /// with the current encoder transform — the RendererGeode layer builds
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
  void fillPathPattern(const Path& path, FillRule rule, const PatternPaint& paint);

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

  /// Shared path for solid and pattern fills: encode path, upload buffers,
  /// build bind group, record draw call.
  void submitFillDraw(const FillDrawArgs& args);

  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::geode
