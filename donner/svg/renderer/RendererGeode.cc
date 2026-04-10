#include "donner/svg/renderer/RendererGeode.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/core/Gradient.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg {

namespace {

constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA8Unorm;

/// The unit path bounds used by `objectBoundingBox` gradient coordinates,
/// matching the helper in RendererTinySkia / RendererSkia.
const Box2d kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

/// Hard cap on gradient stops baked into the uniform buffer. Must be
/// <= `GeoEncoder`'s internal `kMaxGradientStops` (which mirrors the WGSL
/// constant in `slug_gradient.wgsl`). Values beyond this cap are truncated
/// with a one-shot warning; the follow-up is a texture-based stop lookup
/// (see `GeodeGradientCacheComponent` in the Geode design doc).
constexpr size_t kMaxGradientStopsClient = 16;

/// WebGPU requires bytesPerRow alignment to 256 when copying textures to
/// buffers. This rounds the unpadded row width up to the next 256 boundary.
constexpr uint32_t alignBytesPerRow(uint32_t unpadded) {
  constexpr uint32_t kAlign = 256u;
  return (unpadded + kAlign - 1u) & ~(kAlign - 1u);
}

/// Convert an SVG stroke-linecap enum to the donner::LineCap used by
/// Path::strokeToFill.
LineCap toLineCap(StrokeLinecap cap) {
  switch (cap) {
    case StrokeLinecap::Butt: return LineCap::Butt;
    case StrokeLinecap::Round: return LineCap::Round;
    case StrokeLinecap::Square: return LineCap::Square;
  }
  return LineCap::Butt;
}

/// Convert an SVG stroke-linejoin enum to the donner::LineJoin used by
/// Path::strokeToFill. MiterClip and Arcs are specialized SVG2 variants that
/// strokeToFill does not yet distinguish; fall back to Miter (matching the
/// tiny-skia backend's handling of these values).
LineJoin toLineJoin(StrokeLinejoin join) {
  switch (join) {
    case StrokeLinejoin::Miter: return LineJoin::Miter;
    case StrokeLinejoin::MiterClip: return LineJoin::Miter;
    case StrokeLinejoin::Round: return LineJoin::Round;
    case StrokeLinejoin::Bevel: return LineJoin::Bevel;
    case StrokeLinejoin::Arcs: return LineJoin::Miter;
  }
  return LineJoin::Miter;
}

/// Build a donner::StrokeStyle from the SVG StrokeParams.
StrokeStyle toStrokeStyle(const StrokeParams& params) {
  StrokeStyle style;
  style.width = params.strokeWidth;
  style.cap = toLineCap(params.lineCap);
  style.join = toLineJoin(params.lineJoin);
  style.miterLimit = params.miterLimit;
  style.dashArray = params.dashArray;
  style.dashOffset = params.dashOffset;
  style.pathLength = params.pathLength;
  return style;
}

/// Coerce a `Lengthd` into a percent-bearing length when the gradient is in
/// `objectBoundingBox` mode. Mirrors the `toPercent` helper used by
/// RendererTinySkia / RendererSkia for gradient coordinate resolution.
inline Lengthd coerceGradientLength(Lengthd value, bool numbersArePercent) {
  if (!numbersArePercent) {
    return value;
  }
  if (value.unit == Lengthd::Unit::None) {
    value.value *= 100.0;
    value.unit = Lengthd::Unit::Percent;
  }
  return value;
}

/// Resolve a `(x, y)` pair of gradient coordinates against the reference
/// bounds (unit box for objectBoundingBox, viewBox for userSpaceOnUse).
Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Box2d& bounds,
                               bool numbersArePercent) {
  return Vector2d(coerceGradientLength(x, numbersArePercent)
                      .toPixels(bounds, FontMetrics(), Lengthd::Extent::X),
                  coerceGradientLength(y, numbersArePercent)
                      .toPixels(bounds, FontMetrics(), Lengthd::Extent::Y));
}

/// Resolve a single gradient coordinate (used for the radial `r` and `fr`
/// attributes, which are isotropic and don't carry an X/Y axis hint).
inline double resolveGradientCoord(Lengthd value, const Box2d& bounds, bool numbersArePercent) {
  return coerceGradientLength(value, numbersArePercent).toPixels(bounds, FontMetrics());
}

/// Resolve the `gradientTransform` attribute of a gradient into a concrete
/// Transform2d. If the referenced entity has no transform component, returns
/// identity. Mirrors the logic in RendererTinySkia / RendererSkia.
Transform2d resolveGradientTransform(
    const components::ComputedLocalTransformComponent* maybeTransformComponent,
    const Box2d& viewBox) {
  if (maybeTransformComponent == nullptr) {
    return Transform2d();
  }
  const Vector2d origin = maybeTransformComponent->transformOrigin;
  const Transform2d entityFromParent =
      maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics());
  return Transform2d::Translate(origin) * entityFromParent *
         Transform2d::Translate(-origin);
}

/// Resolved frame for either kind of gradient: the bounds against which
/// gradient coordinates are evaluated (`coordBounds`), the path-from-gradient
/// transform (already inverted to `gradientFromPath`), and a `numbersArePercent`
/// flag for objectBoundingBox-mode coordinate coercion.
struct ResolvedGradientFrame {
  Transform2d gradientFromPath;
  Box2d coordBounds;
  bool numbersArePercent = false;
};

/// Resolve the gradient's coordinate frame and `gradientFromPath` transform.
/// Returns nullopt for malformed or degenerate frames (degenerate
/// objectBoundingBox bounds, singular gradientTransform). Shared by the
/// linear and radial resolvers — they only differ in which start/end /
/// center/radius fields they then read from the typed gradient component.
std::optional<ResolvedGradientFrame> resolveGradientFrame(
    const EntityHandle handle, const components::ComputedGradientComponent& computedGradient,
    const Box2d& pathBounds, const Box2d& viewBox) {
  const bool objectBoundingBox =
      computedGradient.gradientUnits == GradientUnits::ObjectBoundingBox;

  // Degenerate path bounds disable objectBoundingBox gradients; ditto the
  // other backends, which bail out rather than produce garbage coordinates.
  constexpr double kDegenerateBBoxTolerance = 1e-6;
  if (objectBoundingBox && (std::abs(pathBounds.width()) < kDegenerateBBoxTolerance ||
                            std::abs(pathBounds.height()) < kDegenerateBBoxTolerance)) {
    return std::nullopt;
  }

  // pathFromGradient maps coordinates expressed in the gradient's own frame
  // (which is either the path's unit bbox or the user-space viewBox) back
  // into the path-space the fragment shader receives. We invert it once on
  // the CPU so the GPU just multiplies row vectors per-pixel.
  //
  // In userSpaceOnUse mode, gradient space == path (user) space modulo the
  // `gradientTransform` attribute, so pathFromGradient == gradientTransform
  // and we can compose directly.
  //
  // In objectBoundingBox mode, coordinates are expressed in the unit box
  // [0..1] relative to the path's bounding box. The mapping back to path
  // space is:
  //   pathFromGradient = bboxFromUnit * gradientTransform
  //                    = Scale(size) · Translate(topLeft) · gradientTransform
  // (post-multiply because the gradientTransform is applied first in the
  // gradient's own local frame.)
  Transform2d pathFromGradient;
  if (objectBoundingBox) {
    const Transform2d gradientTransform = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), kUnitPathBounds);
    const Transform2d bboxFromUnit =
        Transform2d::Scale(pathBounds.size()) * Transform2d::Translate(pathBounds.topLeft);
    pathFromGradient = gradientTransform * bboxFromUnit;
  } else {
    pathFromGradient = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), viewBox);
  }

  if (std::abs(pathFromGradient.determinant()) < 1e-12) {
    return std::nullopt;
  }

  ResolvedGradientFrame frame;
  frame.gradientFromPath = pathFromGradient.inverse();
  frame.coordBounds = objectBoundingBox ? kUnitPathBounds : viewBox;
  frame.numbersArePercent = objectBoundingBox;
  return frame;
}

/// Map an SVG `GradientSpreadMethod` to the encoder's `spreadMode` integer.
inline uint32_t toGeoSpreadMode(GradientSpreadMethod method) {
  switch (method) {
    case GradientSpreadMethod::Pad: return 0u;
    case GradientSpreadMethod::Reflect: return 1u;
    case GradientSpreadMethod::Repeat: return 2u;
  }
  return 0u;
}

/// Translate the gradient's stop list into the encoder wire format. Returns
/// the populated count and sets `*outStopsTruncated` if the source had more
/// stops than the encoder's hard cap.
size_t buildGradientStops(const components::ComputedGradientComponent& computedGradient,
                          const css::RGBA currentColor, float opacity,
                          std::vector<geode::LinearGradientParams::Stop>& stopsStorage,
                          bool* outStopsTruncated) {
  const size_t stopCount = std::min(computedGradient.stops.size(), kMaxGradientStopsClient);
  if (outStopsTruncated != nullptr) {
    *outStopsTruncated = computedGradient.stops.size() > kMaxGradientStopsClient;
  }
  stopsStorage.clear();
  stopsStorage.reserve(stopCount);

  // Per SVG 1.1 §13.2.4 / SVG 2 §12.6.2: each stop's `offset` must be
  // monotonically non-decreasing. If a stop specifies an offset less than
  // the largest previous stop's offset, the offset is clamped up to that
  // largest previous value. Missing offsets effectively default to the
  // previous stop's offset via the same rule. The shader's
  // `sample_stops` assumes monotonic offsets — violating the invariant
  // produces wrong colors on the affected range (e-stop-003, e-stop-024).
  float minOffset = 0.0f;
  for (size_t i = 0; i < stopCount; ++i) {
    const GradientStop& stop = computedGradient.stops[i];
    const css::RGBA rgba = stop.color.resolve(currentColor, stop.opacity * opacity);
    geode::LinearGradientParams::Stop out;
    const float clampedOffset =
        std::clamp<float>(static_cast<float>(stop.offset), 0.0f, 1.0f);
    out.offset = std::max(clampedOffset, minOffset);
    minOffset = out.offset;
    out.rgba[0] = rgba.r / 255.0f;
    out.rgba[1] = rgba.g / 255.0f;
    out.rgba[2] = rgba.b / 255.0f;
    out.rgba[3] = rgba.a / 255.0f;
    stopsStorage.push_back(out);
  }
  return stopCount;
}

/// Attempt to resolve a `PaintResolvedReference` into a concrete linear-gradient
/// draw specification for the Geode encoder. Returns `std::nullopt` for any
/// non-linear / malformed / degenerate gradient; the caller should fall back
/// to the gradient's fallback color or skip the draw.
std::optional<geode::LinearGradientParams> resolveLinearGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds,
    const Box2d& viewBox, const css::RGBA currentColor, float opacity,
    std::vector<geode::LinearGradientParams::Stop>& stopsStorage,
    bool* outStopsTruncated) {
  if (outStopsTruncated != nullptr) {
    *outStopsTruncated = false;
  }

  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }
  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  if (computedGradient == nullptr || !computedGradient->initialized) {
    return std::nullopt;
  }
  const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>();
  if (linear == nullptr) {
    // Not a linear gradient — caller will try the radial resolver next.
    return std::nullopt;
  }

  auto frame = resolveGradientFrame(handle, *computedGradient, pathBounds, viewBox);
  if (!frame.has_value()) {
    return std::nullopt;
  }

  const Vector2d startGrad =
      resolveGradientCoords(linear->x1, linear->y1, frame->coordBounds, frame->numbersArePercent);
  const Vector2d endGrad =
      resolveGradientCoords(linear->x2, linear->y2, frame->coordBounds, frame->numbersArePercent);

  if (computedGradient->stops.empty()) {
    return std::nullopt;
  }

  buildGradientStops(*computedGradient, currentColor, opacity, stopsStorage, outStopsTruncated);

  geode::LinearGradientParams params;
  params.startGrad = startGrad;
  params.endGrad = endGrad;
  params.gradientFromPath = frame->gradientFromPath;
  params.spreadMode = toGeoSpreadMode(computedGradient->spreadMethod);
  params.stops = std::span<const geode::LinearGradientParams::Stop>(stopsStorage);
  return params;
}

/// Result of resolving a radial gradient against a path. Either a fully
/// specified radial gradient ready for the GPU, OR — per SVG2 — a degenerate
/// radial gradient that should be painted as a solid color equal to the
/// last stop. Returning nullopt means "not a radial gradient" (caller should
/// fall through to the next resolver).
struct ResolvedRadialGradient {
  std::optional<geode::RadialGradientParams> gradient;
  std::optional<css::RGBA> solidFallback;
};

/// Same shape as @ref resolveLinearGradientParams, but for radial gradients.
/// Returns `nullopt` if the referenced entity isn't a radial gradient. If it
/// IS a radial gradient but is degenerate (zero outer radius), returns a
/// populated `solidFallback` with the last stop's color — matching
/// RendererSkia (see `RendererSkia.cc:2354`). If the focal circle fully
/// contains the outer circle, returns an empty result (both fields unset)
/// so the caller drops the draw.
std::optional<ResolvedRadialGradient> resolveRadialGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds,
    const Box2d& viewBox, const css::RGBA currentColor, float opacity,
    std::vector<geode::RadialGradientParams::Stop>& stopsStorage, bool* outStopsTruncated) {
  if (outStopsTruncated != nullptr) {
    *outStopsTruncated = false;
  }

  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }
  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  if (computedGradient == nullptr || !computedGradient->initialized) {
    return std::nullopt;
  }
  const auto* radial = handle.try_get<components::ComputedRadialGradientComponent>();
  if (radial == nullptr) {
    return std::nullopt;
  }

  auto frame = resolveGradientFrame(handle, *computedGradient, pathBounds, viewBox);
  if (!frame.has_value()) {
    // Recognized as radial but frame is degenerate (singular transform, etc).
    // Drop the draw — no meaningful output possible.
    return ResolvedRadialGradient{};
  }

  if (computedGradient->stops.empty()) {
    return ResolvedRadialGradient{};
  }

  const double radius =
      resolveGradientCoord(radial->r, frame->coordBounds, frame->numbersArePercent);
  // SVG2: a radius of zero collapses the gradient to a single point. Match
  // RendererSkia's behavior of painting a solid color equal to the LAST
  // stop in the gradient — this keeps elements visible for valid degenerate
  // radial gradients (e.g., `r="0"` with a single visible color).
  if (radius <= 0.0) {
    ResolvedRadialGradient out;
    const auto& lastStop = computedGradient->stops.back();
    const css::RGBA base = lastStop.color.resolve(currentColor, opacity);
    // Multiply in stop-opacity per SVG2 (stop-color * stop-opacity) and then
    // the overall paint opacity factor that `buildGradientStops` also honors.
    const double stopOpacity = std::clamp<double>(lastStop.opacity, 0.0, 1.0);
    out.solidFallback =
        css::RGBA(base.r, base.g, base.b, static_cast<uint8_t>(std::round(base.a * stopOpacity)));
    return out;
  }

  const double focalRadius =
      resolveGradientCoord(radial->fr, frame->coordBounds, frame->numbersArePercent);
  const Vector2d center =
      resolveGradientCoords(radial->cx, radial->cy, frame->coordBounds, frame->numbersArePercent);
  // SVG 2: if `fx` / `fy` aren't specified, they coincide with `cx` / `cy`.
  // Resolved on the spot — keeps the geometry resolution local to the
  // shader's coordinate system.
  const Vector2d focalCenter = resolveGradientCoords(
      radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy), frame->coordBounds,
      frame->numbersArePercent);

  // Empty annulus: focal circle entirely contains the outer circle, so the
  // gradient never has a valid `t` anywhere. Match tiny-skia: drop the draw
  // (no shader can produce meaningful colors).
  const double centerDistance = (center - focalCenter).length();
  if (centerDistance + radius <= focalRadius) {
    return ResolvedRadialGradient{};
  }

  buildGradientStops(*computedGradient, currentColor, opacity, stopsStorage, outStopsTruncated);

  geode::RadialGradientParams params;
  params.center = center;
  params.focalCenter = focalCenter;
  params.radius = radius;
  params.focalRadius = focalRadius;
  params.gradientFromPath = frame->gradientFromPath;
  params.spreadMode = toGeoSpreadMode(computedGradient->spreadMethod);
  params.stops = std::span<const geode::RadialGradientParams::Stop>(stopsStorage);

  ResolvedRadialGradient out;
  out.gradient = std::move(params);
  return out;
}

}  // namespace

struct RendererGeode::Impl {
  bool verbose = false;

  // GPU resources. Created in the constructor; if device creation fails,
  // `device` is null and the renderer enters a no-op state.
  std::unique_ptr<geode::GeodeDevice> device;
  std::unique_ptr<geode::GeodePipeline> pipeline;
  std::unique_ptr<geode::GeodeGradientPipeline> gradientPipeline;
  std::unique_ptr<geode::GeodeImagePipeline> imagePipeline;

  // Per-frame resources, recreated in `beginFrame`.
  RenderViewport viewport;
  int pixelWidth = 0;
  int pixelHeight = 0;
  wgpu::Texture target;  // RGBA8 RenderAttachment | CopySrc
  std::unique_ptr<geode::GeoEncoder> encoder;

  // Reusable scratch storage for gradient stop vectors — keeps the
  // per-fillPath allocation counts down and lets the `std::span` in
  // `LinearGradientParams` remain stable across the call.
  std::vector<geode::LinearGradientParams::Stop> gradientStopScratch;

  // CPU-side state.
  PaintParams paint;
  Transform2d currentTransform;
  std::vector<Transform2d> transformStack;

  // --- Pattern tile state (Phase 2H) ---
  //
  // When the driver calls `beginPatternTile`, we save the active `encoder`
  // and transform state onto `patternStack`, then allocate an offscreen
  // tile texture + a fresh `GeoEncoder` that redirects subsequent draws into
  // it. `endPatternTile` finishes that encoder, pops the saved state, and
  // stashes the resulting texture as the current fill/stroke pattern paint
  // via `patternFillPaint` / `patternStrokePaint`.
  //
  // The Slug fill shader supports pattern sampling directly (paintMode==1),
  // so the subsequent draw call samples the tile through the existing fill
  // pipeline — no separate textured-quad pass is needed and the path's
  // Slug coverage test naturally handles arbitrary (non-rectangular) fills.
  struct PatternStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    Transform2d savedTransform;
    std::vector<Transform2d> savedTransformStack;
    int savedPixelWidth = 0;
    int savedPixelHeight = 0;

    // The pattern tile being recorded.
    Box2d tileRect;                 // In pattern space (topLeft at origin).
    Transform2d targetFromPattern;  // Transform used when the tile is sampled.
    wgpu::Texture tileTexture;      // Offscreen tile being written to.
    int tilePixelWidth = 0;
    int tilePixelHeight = 0;
    // Scale factor applied to all `setTransform` calls while this frame is
    // active, mapping pattern-tile units to tile-texture pixels so the
    // encoder's viewport math works out.
    Vector2d rasterScale = Vector2d(1.0, 1.0);
  };
  std::vector<PatternStackFrame> patternStack;

  /// A saved encoder + target state for an in-progress isolated layer
  /// (`pushIsolatedLayer` / `popIsolatedLayer`). When the driver begins a
  /// group with non-identity opacity or a non-Normal blend mode, we
  /// redirect subsequent draws into an offscreen texture of the same size
  /// as the current target. On `pop`, the offscreen is composited back
  /// onto the saved target with the stored opacity.
  struct LayerStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    wgpu::Texture layerTexture;
    double opacity = 1.0;
  };
  std::vector<LayerStackFrame> layerStack;

  /// A completed pattern tile ready to be sampled as fill or stroke paint.
  struct PatternPaintSlot {
    wgpu::Texture tile;
    Vector2d tileSize;              // In pattern space.
    Transform2d targetFromPattern;  // destFromSource naming.
    // `currentTransform` snapshotted at the time the outer element kicked
    // off `beginPatternTile`. This is the path→device transform that the
    // SAME outer element will use when its fill draw happens. Used at
    // pattern-paint build time to strip the canvas-scale / parent-transform
    // chain back out of the live `currentTransform`, so the resulting
    // `patternFromPath` matrix compares path-space positions against the
    // pattern tile's user-space coordinate system instead of accidentally
    // multiplying them by the viewBox→canvas scale.
    Transform2d deviceFromPathAtCapture;
  };
  std::optional<PatternPaintSlot> patternFillPaint;
  std::optional<PatternPaintSlot> patternStrokePaint;

  /// Axis-aligned clip rectangles in target-pixel coords. Each entry
  /// corresponds to a `pushClip` with a non-empty `clipRect`. The active
  /// scissor is the intersection of every entry on this stack.
  /// Entries with `valid == false` represent pushClip calls that had no
  /// `clipRect` component (path- or mask-only clips) — they're tracked
  /// so `popClip` stays balanced with `pushClip`.
  struct ClipStackEntry {
    Box2d pixelRect;
    bool valid = false;
  };
  std::vector<ClipStackEntry> clipStack;

  /// Recompute the intersection of every rectangular clip entry on
  /// `clipStack` and apply it to the active encoder as a scissor. Called
  /// whenever the clip stack changes.
  void updateEncoderScissor() {
    if (!encoder) {
      return;
    }
    std::optional<Box2d> active;
    for (const ClipStackEntry& entry : clipStack) {
      if (!entry.valid) {
        continue;
      }
      if (!active.has_value()) {
        active = entry.pixelRect;
      } else {
        // Intersect: take the overlap of the two rectangles.
        const double x0 = std::max(active->topLeft.x, entry.pixelRect.topLeft.x);
        const double y0 = std::max(active->topLeft.y, entry.pixelRect.topLeft.y);
        const double x1 = std::min(active->bottomRight.x, entry.pixelRect.bottomRight.x);
        const double y1 = std::min(active->bottomRight.y, entry.pixelRect.bottomRight.y);
        active = Box2d(Vector2d(x0, y0), Vector2d(std::max(x0, x1), std::max(y0, y1)));
      }
    }
    if (!active.has_value()) {
      encoder->clearScissorRect();
      return;
    }
    // Convert the Box2d corners to integer pixel coords. Floor on topLeft
    // and ceil on bottomRight so we don't accidentally clip fractional
    // edge pixels that the rasterizer still wants to cover.
    const int32_t x = static_cast<int32_t>(std::floor(active->topLeft.x));
    const int32_t y = static_cast<int32_t>(std::floor(active->topLeft.y));
    const int32_t w = std::max(
        0, static_cast<int32_t>(std::ceil(active->bottomRight.x)) - x);
    const int32_t h = std::max(
        0, static_cast<int32_t>(std::ceil(active->bottomRight.y)) - y);
    encoder->setScissorRect(x, y, w, h);
  }

  // Stub-state depth counters — incremented on push, decremented on pop. Used
  // only to keep stack semantics balanced and to drop the warning to stderr
  // exactly once per category in verbose mode.
  bool warnedClip = false;
  bool warnedLayer = false;
  bool warnedFilter = false;
  bool warnedMask = false;
  bool warnedGradient = false;
  bool warnedImage = false;
  bool warnedText = false;

  /// Resolve the current fill/stroke paint's fallback to a solid RGBA color
  /// when the referenced paint server can't instantiate. Returns nullopt for
  /// None and for paint-server references without a fallback.
  std::optional<css::RGBA> resolveSolidFill() {
    return resolveSolidPaint(paint.fill, paint.fillOpacity);
  }

  std::optional<css::RGBA> resolveSolidStroke() {
    return resolveSolidPaint(paint.stroke, paint.strokeOpacity);
  }

  std::optional<css::RGBA> resolveSolidPaint(const components::ResolvedPaintServer& server,
                                             double effectiveOpacity) {
    if (std::holds_alternative<PaintServer::None>(server)) {
      return std::nullopt;
    }
    const css::RGBA currentColor = paint.currentColor.rgba();
    const float opacity = static_cast<float>(effectiveOpacity);
    if (const auto* solid = std::get_if<PaintServer::Solid>(&server)) {
      return solid->color.resolve(currentColor, opacity);
    }
    if (const auto* ref = std::get_if<components::PaintResolvedReference>(&server)) {
      if (ref->fallback.has_value()) {
        return ref->fallback->resolve(currentColor, opacity);
      }
    }
    return std::nullopt;
  }

  /// Build a `GeoEncoder::PatternPaint` from a stashed slot, composing the
  /// current transform so the shader samples in the correct space.
  ///
  /// Math: during the fill draw, the Geode vertex shader emits `sample_pos`
  /// in PATH space (pre-MVP, pre-`currentTransform`). The pattern fragment
  /// shader needs pattern-tile-space coordinates. The driver gave us
  /// `targetFromPattern` in USER space (i.e., the viewBox frame the outer
  /// element was drawn in) — *not* device space — which is a semantic
  /// mismatch with the renderer, where `currentTransform` goes all the way
  /// from path space to device pixels (i.e., it bakes in the
  /// viewBox→canvas scale on top of the entity's own transform).
  ///
  /// To bridge the mismatch we capture `deviceFromPathAtCapture = currentTransform`
  /// at `beginPatternTile` time. Both the pattern's content subtree and the
  /// eventual fill draw use the same referencing element, so that transform
  /// is the path→device mapping we'd want for BOTH the tile raster and
  /// the final sample. The chain is:
  ///
  ///   pattern_pos  =  inverse(deviceFromPath_at_capture * targetFromPattern)
  ///                 · currentTransform · path_pos
  ///
  /// Expanding: the two `currentTransform`/`deviceFromPath_at_capture` matrices
  /// cancel (they're the same transform when the outer element is drawing
  /// its own fill immediately after the pattern subtree returns), leaving
  /// `inverse(targetFromPattern) · path_pos` — which sits in the user-space
  /// frame that `tileSize` is expressed in. That keeps the shader's
  /// `fract(patternPos / tileSize)` well-defined regardless of the
  /// viewBox→canvas scale.
  geode::GeoEncoder::PatternPaint buildPatternPaint(const PatternPaintSlot& slot,
                                                    double opacity) const {
    const Transform2d deviceFromPattern =
        slot.deviceFromPathAtCapture * slot.targetFromPattern;
    const Transform2d patternFromDevice = deviceFromPattern.inverse();
    const Transform2d patternFromPath = patternFromDevice * currentTransform;
    geode::GeoEncoder::PatternPaint p;
    p.tile = slot.tile;
    p.tileSize = slot.tileSize;
    p.patternFromPath = patternFromPath;
    p.opacity = opacity;
    return p;
  }

  /// Push the renderer's currentTransform onto the encoder before drawing.
  void syncTransform() {
    if (encoder) {
      encoder->setTransform(currentTransform);
    }
  }

  /// Issue a fill of the given path using the current `paint.fill`. Handles
  /// solid colors, linear and radial gradients, and pattern tiles. None and
  /// unsupported types are ignored or fall back to their fallback color.
  void fillResolved(const Path& path, FillRule rule) {
    if (!encoder) {
      return;
    }
    // Pattern dispatch comes first: a pattern slot is populated by the
    // driver via `endPatternTile`, and is consumed by the very next fill or
    // stroke draw (matching RendererTinySkia/RendererSkia semantics).
    if (patternFillPaint.has_value()) {
      syncTransform();
      const double opacity = paint.fillOpacity;
      encoder->fillPathPattern(path, rule, buildPatternPaint(*patternFillPaint, opacity));
      patternFillPaint.reset();
      return;
    }
    const double effectiveOpacity = paint.fillOpacity;
    drawPaintedPath(path, paint.fill, effectiveOpacity, rule);
  }

  /// Core dispatch: given a path and a resolved paint server, emit the
  /// appropriate fill call (solid color or gradient).
  void drawPaintedPath(const Path& path, const components::ResolvedPaintServer& server,
                       double effectiveOpacity, FillRule rule) {
    drawPaintedPathAgainst(path, path, server, effectiveOpacity, rule);
  }

  /// Same as `drawPaintedPath`, but the gradient's objectBoundingBox is
  /// computed from `geometryPath` while the GPU draw uses `drawPath`. This
  /// is required for stroked outlines: SVG specifies that the
  /// `objectBoundingBox` of a stroke gradient is derived from the *original*
  /// geometry, not the expanded stroke outline, otherwise a thick stroke
  /// would warp the gradient direction relative to the underlying shape.
  void drawPaintedPathAgainst(const Path& geometryPath, const Path& drawPath,
                              const components::ResolvedPaintServer& server,
                              double effectiveOpacity, FillRule rule) {
    if (!encoder || drawPath.empty()) {
      return;
    }
    if (std::holds_alternative<PaintServer::None>(server)) {
      return;
    }

    const css::RGBA currentColor = paint.currentColor.rgba();
    const float opacity = static_cast<float>(effectiveOpacity);

    // Solid color: straight through the flat fill pipeline.
    if (const auto* solid = std::get_if<PaintServer::Solid>(&server)) {
      syncTransform();
      encoder->fillPath(drawPath, solid->color.resolve(currentColor, opacity), rule);
      return;
    }

    // Paint-server reference: try linear gradient first, then radial; if
    // neither matches, fall back to the reference's solid fallback color.
    if (const auto* ref = std::get_if<components::PaintResolvedReference>(&server)) {
      const Box2d geometryBounds = geometryPath.bounds();
      bool stopsTruncated = false;

      auto linear = resolveLinearGradientParams(*ref, geometryBounds, paint.viewBox, currentColor,
                                                opacity, gradientStopScratch, &stopsTruncated);
      if (linear.has_value()) {
        if (stopsTruncated && verbose && !warnedGradient) {
          std::cerr << "RendererGeode: gradient has more than " << kMaxGradientStopsClient
                    << " stops; truncating (follow-up: texture-based stop lookup)\n";
          warnedGradient = true;
        }
        syncTransform();
        encoder->fillPathLinearGradient(drawPath, *linear, rule);
        return;
      }

      auto radial = resolveRadialGradientParams(*ref, geometryBounds, paint.viewBox, currentColor,
                                                opacity, gradientStopScratch, &stopsTruncated);
      if (radial.has_value()) {
        if (stopsTruncated && verbose && !warnedGradient) {
          std::cerr << "RendererGeode: gradient has more than " << kMaxGradientStopsClient
                    << " stops; truncating (follow-up: texture-based stop lookup)\n";
          warnedGradient = true;
        }
        if (radial->gradient.has_value()) {
          syncTransform();
          encoder->fillPathRadialGradient(drawPath, *radial->gradient, rule);
          return;
        }
        if (radial->solidFallback.has_value()) {
          // SVG2 degenerate radial (r=0): paint the last stop color as a
          // solid fill so the element remains visible.
          syncTransform();
          encoder->fillPath(drawPath, *radial->solidFallback, rule);
          return;
        }
        // Recognized as radial but otherwise unusable (empty stops, focal
        // circle containing outer, singular transform, degenerate
        // objectBoundingBox frame). Fall through to the paint-server
        // fallback below — per SVG2, a gradient paint server that can't
        // be instantiated on a given element should use the reference's
        // fallback color (e.g., `stroke="url(#lg) green"` paints green on
        // a zero-height horizontal line where the objectBoundingBox
        // gradient can't be applied).
      }

      // Neither linear nor radial — could be a pattern, a sweep gradient
      // (not yet supported by the donner SVG parser), a malformed gradient
      // with no stops, or a degenerate frame. Fall back to the reference's
      // solid fallback color if one was declared, otherwise drop the draw.
      if (ref->fallback.has_value()) {
        syncTransform();
        encoder->fillPath(drawPath, ref->fallback->resolve(currentColor, opacity), rule);
        return;
      }

      // No fallback, no gradient support — issue a one-shot warning so
      // verbose callers can see it.
      if (verbose && !warnedGradient) {
        std::cerr << "RendererGeode: paint server is neither linear nor radial gradient and "
                     "has no fallback (patterns and sweep gradients are Phase 2H+)\n";
        warnedGradient = true;
      }
    }
  }
};

RendererGeode::RendererGeode(bool verbose) : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  impl_->device = geode::GeodeDevice::CreateHeadless();
  if (!impl_->device) {
    if (verbose) {
      std::cerr << "RendererGeode: GeodeDevice::CreateHeadless() failed — entering no-op mode\n";
    }
    return;
  }
  impl_->pipeline = std::make_unique<geode::GeodePipeline>(impl_->device->device(), kFormat);
  impl_->gradientPipeline =
      std::make_unique<geode::GeodeGradientPipeline>(impl_->device->device(), kFormat);
  impl_->imagePipeline =
      std::make_unique<geode::GeodeImagePipeline>(impl_->device->device(), kFormat);
}

RendererGeode::~RendererGeode() = default;
RendererGeode::RendererGeode(RendererGeode&&) noexcept = default;
RendererGeode& RendererGeode::operator=(RendererGeode&&) noexcept = default;

void RendererGeode::draw(SVGDocument& document) {
  RendererDriver driver(*this, impl_->verbose);
  driver.draw(document);
}

int RendererGeode::width() const { return impl_->pixelWidth; }
int RendererGeode::height() const { return impl_->pixelHeight; }

void RendererGeode::beginFrame(const RenderViewport& viewport) {
  impl_->viewport = viewport;
  impl_->pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  impl_->pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);
  impl_->currentTransform = Transform2d();
  impl_->transformStack.clear();
  impl_->paint = PaintParams();
  impl_->encoder.reset();
  impl_->target = wgpu::Texture();

  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return;
  }

  wgpu::TextureDescriptor td = {};
  td.label = "RendererGeodeTarget";
  td.size = {static_cast<uint32_t>(impl_->pixelWidth),
             static_cast<uint32_t>(impl_->pixelHeight), 1};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::e2D;
  impl_->target = impl_->device->device().CreateTexture(&td);

  impl_->encoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->target);
  // Default to a transparent clear so an empty frame matches the other
  // backends' "no document content" appearance.
  impl_->encoder->clear(css::RGBA(0, 0, 0, 0));
}

void RendererGeode::endFrame() {
  if (impl_->encoder) {
    impl_->encoder->finish();
    impl_->encoder.reset();
  }
  impl_->currentTransform = Transform2d();
  impl_->transformStack.clear();
}

void RendererGeode::setTransform(const Transform2d& transform) {
  // Inside a pattern tile we're rasterising into a texture whose pixel
  // dimensions may not match the logical tile-space units. Pre-compose the
  // raster scale onto the transform so draws submitted to the tile encoder
  // map 1:1 onto the tile texture's pixel grid. (Matches
  // `RendererTinySkia::setTransform`'s `scaleTransformOutput` path.)
  if (!impl_->patternStack.empty()) {
    const Vector2d& scale = impl_->patternStack.back().rasterScale;
    Transform2d scaled = transform;
    scaled.data[0] *= scale.x;
    scaled.data[2] *= scale.x;
    scaled.data[4] *= scale.x;
    scaled.data[1] *= scale.y;
    scaled.data[3] *= scale.y;
    scaled.data[5] *= scale.y;
    impl_->currentTransform = scaled;
    return;
  }
  impl_->currentTransform = transform;
}

void RendererGeode::pushTransform(const Transform2d& transform) {
  impl_->transformStack.push_back(impl_->currentTransform);
  impl_->currentTransform = transform * impl_->currentTransform;
}

void RendererGeode::popTransform() {
  if (impl_->transformStack.empty()) {
    return;
  }
  impl_->currentTransform = impl_->transformStack.back();
  impl_->transformStack.pop_back();
}

void RendererGeode::pushClip(const ResolvedClip& clip) {
  // Rectangular clip (the nested-`<svg>` viewport, `overflow: hidden`, and
  // `<image>` dest-rect cases) is implemented via the WebGPU scissor rect.
  // Path- and mask-based clipping are still stubbed — they require a
  // stencil or mask-texture pass which is Phase 3 proper.
  const bool hasPathOrMaskClip = !clip.clipPaths.empty() || clip.mask.has_value();
  if (hasPathOrMaskClip && impl_->verbose && !impl_->warnedClip) {
    std::cerr << "RendererGeode: path/mask clipping not yet implemented (Phase 3)\n";
    impl_->warnedClip = true;
  }

  // Compose the incoming clip rect (in user-space) with the current
  // transform to get pixel-space coordinates, then push onto the stack.
  // The active scissor is the INTERSECTION of everything on the stack.
  Box2d pixelRect;
  bool valid = false;
  if (clip.clipRect.has_value()) {
    const Transform2d& t = impl_->currentTransform;
    pixelRect = t.transformBox(*clip.clipRect);
    valid = true;
  }
  impl_->clipStack.push_back({pixelRect, valid});
  impl_->updateEncoderScissor();
}

void RendererGeode::popClip() {
  if (!impl_->clipStack.empty()) {
    impl_->clipStack.pop_back();
  }
  impl_->updateEncoderScissor();
}

void RendererGeode::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  // Blend modes other than Normal are a Phase 7 concern (they require the
  // compute-based filter / compositor pipeline). For now we support
  // Isolation + alpha — which handles group opacity, `<svg opacity=...>`,
  // and nested-group `opacity` propagation, covering a-opacity-001/007/008
  // and several adjacent tests. Anything-but-Normal blend is dropped with
  // a one-shot warning.
  if (blendMode != MixBlendMode::Normal) {
    if (impl_->verbose && !impl_->warnedLayer) {
      std::cerr << "RendererGeode: non-Normal blend modes not yet implemented (Phase 7)\n";
      impl_->warnedLayer = true;
    }
  }
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline ||
      !impl_->imagePipeline || !impl_->encoder) {
    // Headless or degenerate state — drop silently but still push a
    // placeholder frame so popIsolatedLayer stays balanced.
    impl_->layerStack.push_back({});
    return;
  }

  // Allocate an offscreen texture of the same size as the current target.
  // All draws issued between push/pop land here instead of the outer
  // target; pop composites this texture back with the stored opacity.
  wgpu::TextureDescriptor td = {};
  td.label = "RendererGeodeIsolatedLayer";
  td.size = {static_cast<uint32_t>(impl_->pixelWidth),
             static_cast<uint32_t>(impl_->pixelHeight), 1u};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture layerTexture = impl_->device->device().CreateTexture(&td);
  if (!layerTexture) {
    impl_->layerStack.push_back({});
    return;
  }

  // Finish the outer encoder so its queued draws land on the saved target
  // before we redirect subsequent work into the offscreen layer. Same
  // shape as `beginPatternTile` — two render-pass submissions ordered
  // serially on the queue.
  impl_->encoder->finish();

  Impl::LayerStackFrame frame;
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.layerTexture = layerTexture;
  frame.opacity = opacity;

  impl_->target = layerTexture;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      layerTexture);
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);
  impl_->layerStack.push_back(std::move(frame));
  // The layer inherits the outer clip stack — reapply it to the new
  // encoder so scissors carry through.
  impl_->updateEncoderScissor();
}

void RendererGeode::popIsolatedLayer() {
  if (impl_->layerStack.empty()) {
    return;
  }
  Impl::LayerStackFrame frame = std::move(impl_->layerStack.back());
  impl_->layerStack.pop_back();

  if (!frame.layerTexture) {
    return;  // Placeholder frame from the headless/error path.
  }

  // Finish the layer's render pass so the texture contents are ready.
  if (impl_->encoder) {
    impl_->encoder->finish();
  }

  // Restore outer target + create a fresh encoder that preserves its
  // existing contents (LoadOp::Load). Draw the layer texture across the
  // entire target with the stored opacity as the compositing alpha.
  impl_->target = frame.savedTarget;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.savedTarget);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();
  impl_->encoder->blitFullTarget(frame.layerTexture, frame.opacity);
}

void RendererGeode::pushFilterLayer(const components::FilterGraph& /*filterGraph*/,
                                    const std::optional<Box2d>& /*filterRegion*/) {
  if (impl_->verbose && !impl_->warnedFilter) {
    std::cerr << "RendererGeode: filter layers not yet implemented (Phase 7)\n";
    impl_->warnedFilter = true;
  }
}

void RendererGeode::popFilterLayer() {}

void RendererGeode::pushMask(const std::optional<Box2d>& /*maskBounds*/) {
  if (impl_->verbose && !impl_->warnedMask) {
    std::cerr << "RendererGeode: masks not yet implemented (Phase 3)\n";
    impl_->warnedMask = true;
  }
}

void RendererGeode::transitionMaskToContent() {}
void RendererGeode::popMask() {}

void RendererGeode::beginPatternTile(const Box2d& tileRect,
                                     const Transform2d& targetFromPattern) {
  if (!impl_->device || !impl_->pipeline) {
    return;
  }

  // Raster resolution: use the composition of the current transform and the
  // tile transform to estimate how many device pixels one tile unit maps to,
  // then scale the tile texture accordingly. Using device pixels directly as
  // a 1:1 fallback would under-sample patterns that are scaled up before
  // tiling. We clamp to a minimum of 1 pixel per axis so zero-size tiles
  // never allocate a zero-extent texture.
  const Transform2d deviceFromPattern = impl_->currentTransform * targetFromPattern;
  const double scaleX = std::hypot(deviceFromPattern.data[0], deviceFromPattern.data[1]);
  const double scaleY = std::hypot(deviceFromPattern.data[2], deviceFromPattern.data[3]);
  auto boundedPx = [](double v) {
    if (!(v > 0.0) || !std::isfinite(v)) {
      return 1;
    }
    constexpr double kMaxTileDim = 4096.0;
    return std::max(1, static_cast<int>(std::ceil(std::min(v, kMaxTileDim))));
  };
  const int tilePixelWidth = boundedPx(tileRect.width() * (scaleX > 0.0 ? scaleX : 1.0));
  const int tilePixelHeight = boundedPx(tileRect.height() * (scaleY > 0.0 ? scaleY : 1.0));

  wgpu::TextureDescriptor td = {};
  td.label = "RendererGeodePatternTile";
  td.size = {static_cast<uint32_t>(tilePixelWidth),
             static_cast<uint32_t>(tilePixelHeight), 1u};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture tileTexture = impl_->device->device().CreateTexture(&td);
  if (!tileTexture) {
    return;
  }

  // Stash the currently-active encoder/target/transform state. A nested
  // encoder can't share a render pass with the outer one, so we finish any
  // pending outer work below. `GeoEncoder` submits its command buffer on
  // `finish()`, so finishing the outer encoder here commits its queued
  // draws; we then create a fresh outer encoder on pop (see
  // `endPatternTile`).
  //
  // An alternate design would share a single command encoder and use two
  // render passes, but `GeoEncoder` owns its command encoder and render
  // pass together. Splitting into two submissions keeps the `GeoEncoder`
  // API unchanged and still orders correctly because each submission is
  // serialised on the same queue.
  Impl::PatternStackFrame frame;
  if (impl_->encoder) {
    impl_->encoder->finish();
  }
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.savedTransform = impl_->currentTransform;
  frame.savedTransformStack = std::move(impl_->transformStack);
  frame.savedPixelWidth = impl_->pixelWidth;
  frame.savedPixelHeight = impl_->pixelHeight;
  frame.tileRect = tileRect;
  frame.targetFromPattern = targetFromPattern;
  frame.tileTexture = tileTexture;
  frame.tilePixelWidth = tilePixelWidth;
  frame.tilePixelHeight = tilePixelHeight;
  // Map pattern-tile units onto tile-texture pixels: this factor is applied
  // to every `setTransform` call while this frame is on the stack so the
  // encoder's pixelWidth/pixelHeight-based MVP renders the tile at its
  // native resolution.
  const double tileWidthUnits = tileRect.width();
  const double tileHeightUnits = tileRect.height();
  frame.rasterScale = Vector2d(
      tileWidthUnits > 0.0 ? static_cast<double>(tilePixelWidth) / tileWidthUnits : 1.0,
      tileHeightUnits > 0.0 ? static_cast<double>(tilePixelHeight) / tileHeightUnits : 1.0);
  impl_->patternStack.push_back(std::move(frame));

  // Redirect all subsequent draw calls into the new tile texture. The new
  // encoder uses a coordinate system where path-space (0,0)..(tileRect.w,
  // tileRect.h) maps to (0,0)..(tilePixelWidth, tilePixelHeight) — i.e.,
  // the tile is rasterised at its native pixel resolution.
  //
  // The driver calls `setTransform` on the renderer before issuing draws
  // inside the tile subtree, so we don't need to preserve the outer
  // transform here. But we do need `pixelWidth/pixelHeight` to match the
  // tile texture so the new encoder's MVP maps correctly.
  impl_->pixelWidth = tilePixelWidth;
  impl_->pixelHeight = tilePixelHeight;
  impl_->target = tileTexture;
  impl_->transformStack.clear();
  // Initialise the current transform to the raster scale so direct draws
  // issued before the driver's next `setTransform` still land in the
  // correct place on the tile texture.
  const Vector2d& rasterScale = impl_->patternStack.back().rasterScale;
  impl_->currentTransform = Transform2d::Scale(rasterScale.x, rasterScale.y);

  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      tileTexture);
  // Transparent clear so unpainted tile pixels contribute nothing.
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);

  // Compose the pattern raster scale into targetFromPattern so it takes
  // pattern-pixel coordinates to target units. We record the raster scale
  // in the frame itself and apply it in endPatternTile.
}

void RendererGeode::endPatternTile(bool forStroke) {
  if (impl_->patternStack.empty()) {
    return;
  }

  Impl::PatternStackFrame frame = std::move(impl_->patternStack.back());
  impl_->patternStack.pop_back();

  // Finish the tile's render pass so the texture contents are available
  // for sampling by subsequent draws.
  if (impl_->encoder) {
    impl_->encoder->finish();
  }

  // Restore outer state.
  impl_->target = frame.savedTarget;
  impl_->pixelWidth = frame.savedPixelWidth;
  impl_->pixelHeight = frame.savedPixelHeight;
  impl_->currentTransform = frame.savedTransform;
  impl_->transformStack = std::move(frame.savedTransformStack);

  // Create a fresh encoder for the outer target. The old outer encoder was
  // finished in `beginPatternTile`; the new one loads the current contents
  // of the target (no clear), since we don't want to wipe previously-drawn
  // content. `GeoEncoder` defaults to clearing unless `hasExplicitClear` is
  // false and no draws are issued — but its render pass *always* uses
  // LoadOp::Clear with a transparent clearColor by default. That would
  // wipe the outer target on the next draw. Work around this by using the
  // saved encoder path: we can't directly reuse the saved encoder because
  // it was finished; instead, manually load the previous contents via a
  // separate copy pass is overkill for the MVP.
  //
  // Instead we issue a `CopyTextureToTexture` before the new encoder's
  // first draw to preserve the outer target's contents... actually an
  // even simpler fix is to have the new encoder skip its default clear by
  // explicitly calling `clear()` — but clear() sets the clearColor which
  // still wipes the target.
  //
  // Simpler: run the pre-existing content through. We allocate a *scratch*
  // intermediate target, copy old contents in, create the new encoder on
  // the scratch, then merge... that's a lot of work.
  //
  // Practical fix: change the render pass load op to Load instead of Clear
  // when there's been at least one prior submission. This requires
  // extending GeoEncoder; see the `reopen` helper added below.
  if (impl_->device && impl_->pipeline && impl_->gradientPipeline && impl_->imagePipeline &&
      frame.savedTarget) {
    auto newEncoder = std::make_unique<geode::GeoEncoder>(
        *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
        frame.savedTarget);
    // Preserve existing target contents: the pattern subtree may have
    // submitted work on the outer target *before* the pattern tile opened
    // (via the finish() in beginPatternTile), so we must not clear it
    // again. GeoEncoder's constructor defaults to LoadOp::Clear with a
    // transparent clearValue; call setLoadPreserve() to switch it to
    // LoadOp::Load for the next render pass.
    newEncoder->setLoadPreserve();
    impl_->encoder = std::move(newEncoder);
  } else {
    impl_->encoder.reset();
  }

  // Stash the completed tile as the current pattern paint slot. The tile
  // size is in *pattern-space* units (matching the vertex shader's
  // input), not pixels — the shader scales `patternFromPath` positions
  // through the existing 4x4 matrix multiply and compares against the
  // tile's native dimensions.
  Impl::PatternPaintSlot slot;
  slot.tile = frame.tileTexture;
  slot.tileSize = frame.tileRect.size();
  slot.targetFromPattern = frame.targetFromPattern;
  // `frame.savedTransform` is the path→device transform that was live at
  // `beginPatternTile` time — i.e., the outer element's currentTransform
  // including the viewBox→canvas scale. We stash it on the slot so
  // `buildPatternPaint` can cancel it out of the live `currentTransform`
  // at the upcoming fill draw, leaving the pattern sample in the pattern's
  // user-space frame (where `tileSize` is expressed).
  slot.deviceFromPathAtCapture = frame.savedTransform;
  if (forStroke) {
    impl_->patternStrokePaint = std::move(slot);
  } else {
    impl_->patternFillPaint = std::move(slot);
  }
}

void RendererGeode::setPaint(const PaintParams& paint) { impl_->paint = paint; }

void RendererGeode::drawPath(const PathShape& path, const StrokeParams& stroke) {
  impl_->fillResolved(path.path, path.fillRule);

  // Mirror fillResolved's no-op safety: if there's no encoder (headless
  // device init failed, zero-pixel viewport, or draw-before-beginFrame),
  // the stroke branch must bail too. Otherwise the encoder dereference
  // below crashes.
  if (!impl_->encoder) {
    return;
  }

  if (stroke.strokeWidth <= 0.0 ||
      std::holds_alternative<PaintServer::None>(impl_->paint.stroke)) {
    return;
  }

  // Dash support is now wired through `Path::strokeToFill` (Phase 2A).
  // `toStrokeStyle` plumbs `dashArray`/`dashOffset`/`pathLength` from the
  // SVG stroke params into the stroke style; `strokeToFill` walks each
  // subpath, splits it at the dash on/off transitions, and emits one
  // capped ribbon per on-segment.
  //
  // Expand the stroked outline into a filled path and reuse the Slug fill /
  // gradient-fill / pattern pipeline. `strokeToFill` handles flattening,
  // cap/join generation, and miter-limit fallback to bevel internally.
  //
  // Fill rule note: for closed subpaths, strokeToFill emits the outer and
  // inner contours as two *same-winding* closed subpaths (not opposite), so
  // NonZero would over-fill the interior and EvenOdd is required to get a
  // hollow ring. EvenOdd is also correct for open subpaths, including sharp
  // inside corners: as of the 2C fix, `emitJoin` emits the true offset-line
  // intersection at inside turns (instead of an interior-crossing overlap),
  // so the resulting single polygon is geometrically clean on both sides.
  const Path strokedOutline = path.path.strokeToFill(toStrokeStyle(stroke));
  if (strokedOutline.empty()) {
    return;
  }

  // Pattern dispatch comes first: a stroke pattern slot was populated by the
  // driver via `endPatternTile(forStroke=true)` and consumed here.
  if (impl_->patternStrokePaint.has_value()) {
    impl_->syncTransform();
    const double opacity = impl_->paint.strokeOpacity;
    impl_->encoder->fillPathPattern(
        strokedOutline, FillRule::EvenOdd,
        impl_->buildPatternPaint(*impl_->patternStrokePaint, opacity));
    impl_->patternStrokePaint.reset();
    return;
  }

  // Otherwise dispatch through the shared painted-path routine so stroke
  // gradients get the same handling as fill gradients (including the
  // gradient-unit objectBoundingBox transform, which is relative to the
  // *original* path bounds — we deliberately do NOT use
  // `strokedOutline.bounds()` here).
  const double effectiveOpacity = impl_->paint.strokeOpacity;
  auto strokeServer = impl_->paint.stroke;
  impl_->drawPaintedPathAgainst(path.path, strokedOutline, strokeServer, effectiveOpacity,
                                FillRule::EvenOdd);
}

void RendererGeode::drawRect(const Box2d& rect, const StrokeParams& stroke) {
  Path path = PathBuilder().addRect(rect).build();
  PathShape shape{std::move(path), FillRule::NonZero, Transform2d(), 0};
  drawPath(shape, stroke);
}

void RendererGeode::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
  Path path = PathBuilder().addEllipse(bounds).build();
  PathShape shape{std::move(path), FillRule::NonZero, Transform2d(), 0};
  drawPath(shape, stroke);
}

void RendererGeode::drawImage(const ImageResource& image, const ImageParams& params) {
  if (!impl_->encoder) {
    return;
  }
  // The element's own `opacity` is handled by `pushIsolatedLayer` in the
  // driver before this call lands, so we do NOT multiply it back in here
  // (doing so would double-apply the group opacity, producing opacity²).
  // Match RendererTinySkia's behavior in `drawImage`: use `params.opacity`
  // (which is the image-specific opacity component) without the
  // paint.opacity factor.
  const double combinedOpacity = params.opacity;
  if (combinedOpacity <= 0.0) {
    return;
  }
  impl_->syncTransform();
  impl_->encoder->drawImage(image, params.targetRect, combinedOpacity,
                            params.imageRenderingPixelated);
}

void RendererGeode::drawText(Registry& /*registry*/,
                             const components::ComputedTextComponent& /*text*/,
                             const TextParams& /*params*/) {
  if (impl_->verbose && !impl_->warnedText) {
    std::cerr << "RendererGeode: text rendering not yet implemented (Phase 4)\n";
    impl_->warnedText = true;
  }
}

RendererBitmap RendererGeode::takeSnapshot() const {
  RendererBitmap bitmap;
  if (!impl_->device || !impl_->target || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return bitmap;
  }

  const uint32_t width = static_cast<uint32_t>(impl_->pixelWidth);
  const uint32_t height = static_cast<uint32_t>(impl_->pixelHeight);
  const uint32_t bytesPerRow = alignBytesPerRow(width * 4u);

  // Allocate readback buffer.
  wgpu::BufferDescriptor bd = {};
  bd.label = "RendererGeodeReadback";
  bd.size = static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height);
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = impl_->device->device().CreateBuffer(&bd);

  // Copy texture → readback buffer.
  wgpu::CommandEncoder enc = impl_->device->device().CreateCommandEncoder();
  wgpu::TexelCopyTextureInfo src = {};
  src.texture = impl_->target;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};
  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = readback;
  dst.layout.bytesPerRow = bytesPerRow;
  dst.layout.rowsPerImage = height;
  wgpu::Extent3D copySize = {width, height, 1};
  enc.CopyTextureToBuffer(&src, &dst, &copySize);

  wgpu::CommandBuffer cmd = enc.Finish();
  impl_->device->queue().Submit(1, &cmd);

  // Map for read.
  bool mapDone = false;
  bool mapOk = false;
  readback.MapAsync(wgpu::MapMode::Read, 0, bd.size, wgpu::CallbackMode::AllowSpontaneous,
                    [&](wgpu::MapAsyncStatus status, wgpu::StringView /*msg*/) {
                      mapOk = (status == wgpu::MapAsyncStatus::Success);
                      mapDone = true;
                    });
  while (!mapDone) {
    impl_->device->device().Tick();
  }
  if (!mapOk) {
    return bitmap;
  }

  const uint8_t* mapped = static_cast<const uint8_t*>(readback.GetConstMappedRange());

  // Strip row padding and unpremultiply alpha so the consumer gets a tightly
  // packed *straight-alpha* RGBA buffer. `GeoEncoder::fillPath` premultiplies
  // paint RGB by alpha before upload to match the blend pipeline's
  // premultiplied storage, but `RendererBitmap` — like Skia's and
  // tiny-skia's `takeSnapshot()` outputs — is defined as straight RGBA.
  // Returning raw texture bytes would darken semi-transparent content and
  // break cross-backend parity.
  bitmap.dimensions = Vector2i(static_cast<int>(width), static_cast<int>(height));
  bitmap.rowBytes = static_cast<size_t>(width) * 4u;
  bitmap.pixels.resize(bitmap.rowBytes * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* srcRow = mapped + static_cast<size_t>(y) * bytesPerRow;
    uint8_t* dstRow = bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t srcR = srcRow[x * 4 + 0];
      const uint8_t srcG = srcRow[x * 4 + 1];
      const uint8_t srcB = srcRow[x * 4 + 2];
      const uint8_t srcA = srcRow[x * 4 + 3];
      if (srcA == 0u) {
        dstRow[x * 4 + 0] = 0u;
        dstRow[x * 4 + 1] = 0u;
        dstRow[x * 4 + 2] = 0u;
        dstRow[x * 4 + 3] = 0u;
        continue;
      }
      if (srcA == 255u) {
        dstRow[x * 4 + 0] = srcR;
        dstRow[x * 4 + 1] = srcG;
        dstRow[x * 4 + 2] = srcB;
        dstRow[x * 4 + 3] = 255u;
        continue;
      }
      // Round-nearest unpremultiply: straight = (premul * 255 + alpha/2) / alpha.
      const unsigned a = srcA;
      const unsigned half = a >> 1u;
      dstRow[x * 4 + 0] = static_cast<uint8_t>(std::min(255u, (srcR * 255u + half) / a));
      dstRow[x * 4 + 1] = static_cast<uint8_t>(std::min(255u, (srcG * 255u + half) / a));
      dstRow[x * 4 + 2] = static_cast<uint8_t>(std::min(255u, (srcB * 255u + half) / a));
      dstRow[x * 4 + 3] = srcA;
    }
  }
  readback.Unmap();
  return bitmap;
}

}  // namespace donner::svg
