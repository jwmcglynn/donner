#include "donner/svg/renderer/RendererGeode.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Path.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/core/Gradient.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathCacheComponent.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/resources/ImageResource.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/base/MathUtils.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"
#endif

namespace donner::svg {

// Pull the Geode-local label helper into this namespace so that the many
// `.label = wgpuLabel("…")` sites below can stay unqualified. See
// GeodeWgpuUtil.h for the helper rationale.
using ::donner::geode::wgpuLabel;

namespace {

// Render-target texture format stored per-instance in Impl::textureFormat (initialized from
// GeodeDevice). Filter-engine intermediate textures always use RGBA8Unorm for compute-shader
// compatibility regardless of the host format.
constexpr wgpu::TextureFormat kFilterIntermediateFormat = wgpu::TextureFormat::RGBA8Unorm;

/// The unit path bounds used by `objectBoundingBox` gradient coordinates,
/// matching the CPU-renderer helper.
const Box2d kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

/// Apply a `Transform2d` to every control point of a `Path`, returning a
/// new `Path` whose commands mirror the input but whose coordinates are
/// pre-transformed. Needed because `GeoEncoder::fillPath` draws in the
/// encoder's current MVP and does not take a separate per-path matrix;
/// for text we want to translate/rotate each glyph's outline before
/// handing it to the encoder.
Path transformPath(const Path& input, const Transform2d& transform) {
  PathBuilder builder;
  const auto points = input.points();
  for (const Path::Command& command : input.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(transform.transformPosition(points[command.pointIndex]),
                        transform.transformPosition(points[command.pointIndex + 1]),
                        transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }
  return builder.build();
}

#ifdef DONNER_TEXT_ENABLED
TextLayoutParams toTextLayoutParams(const TextParams& params) {
  TextLayoutParams layoutParams;
  layoutParams.fontFamilies = params.fontFamilies;
  layoutParams.fontSize = params.fontSize;
  layoutParams.viewBox = params.viewBox;
  layoutParams.fontMetrics = params.fontMetrics;
  layoutParams.textAnchor = params.textAnchor;
  layoutParams.dominantBaseline = params.dominantBaseline;
  layoutParams.writingMode = params.writingMode;
  layoutParams.letterSpacingPx = params.letterSpacingPx;
  layoutParams.wordSpacingPx = params.wordSpacingPx;
  layoutParams.textLength = params.textLength;
  layoutParams.lengthAdjust = params.lengthAdjust;
  return layoutParams;
}
#endif

/// Hard cap on gradient stops baked into the uniform buffer. Must be
/// <= `GeoEncoder`'s internal `kMaxGradientStops` (which mirrors the WGSL
/// constant in `slug_gradient.wgsl`). Values beyond this cap are truncated
/// with a one-shot warning; the follow-up is a texture-based stop lookup
/// (see `GeodeGradientCacheComponent` in the Geode design doc).
constexpr size_t kMaxGradientStopsClient = 16;

/// Returns true when the filter graph contains spatial-shift primitives (feOffset) that can bring
/// content from outside the viewport into view, requiring the filter layer buffer to be expanded.
bool graphHasSpatialShift(const components::FilterGraph& filterGraph) {
  using namespace components::filter_primitive;
  for (const components::FilterNode& node : filterGraph.nodes) {
    if (std::holds_alternative<Offset>(node.primitive)) {
      return true;
    }
  }
  return false;
}

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
/// `objectBoundingBox` mode. Mirrors the helper used by the software renderer
/// for gradient coordinate resolution.
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
Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Box2d& bounds, bool numbersArePercent) {
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
/// identity. Mirrors the logic in the software renderer.
Transform2d resolveGradientTransform(
    const components::ComputedLocalTransformComponent* maybeTransformComponent,
    const Box2d& viewBox) {
  if (maybeTransformComponent == nullptr) {
    return Transform2d();
  }
  const Vector2d origin = maybeTransformComponent->transformOrigin;
  const Transform2d parentFromEntity =
      maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics());
  return Transform2d::Translate(origin) * parentFromEntity * Transform2d::Translate(-origin);
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
  const bool objectBoundingBox = computedGradient.gradientUnits == GradientUnits::ObjectBoundingBox;

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
    const float clampedOffset = std::clamp<float>(static_cast<float>(stop.offset), 0.0f, 1.0f);
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
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
    const css::RGBA currentColor, float opacity,
    std::vector<geode::LinearGradientParams::Stop>& stopsStorage, bool* outStopsTruncated) {
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
/// populated `solidFallback` with the last stop's color, matching the removed
/// full-Skia backend. If the focal circle fully
/// contains the outer circle, returns an empty result (both fields unset)
/// so the caller drops the draw.
std::optional<ResolvedRadialGradient> resolveRadialGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
    const css::RGBA currentColor, float opacity,
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
  // the removed full-Skia backend's behavior of painting a solid color equal to the LAST
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
  const Vector2d focalCenter =
      resolveGradientCoords(radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy),
                            frame->coordBounds, frame->numbersArePercent);

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

  // Per-frame perf counters. See design doc 0030 (geode_performance) for
  // the tracked sites and the optimization milestones these drive.
  // Reset at `beginFrame`, read via `lastFrameTimings()`.
  geode::GeodeCounters counters;

  // GPU resources. Created in the constructor; if device creation fails,
  // `device` is null and the renderer enters a no-op state.
  //
  // Held via shared_ptr so that test fixtures can share a single GeodeDevice
  // across many short-lived renderer instances (see RendererTestBackendGeode).
  std::shared_ptr<geode::GeodeDevice> device;
  std::unique_ptr<geode::GeodePipeline> pipeline;
  std::unique_ptr<geode::GeodeGradientPipeline> gradientPipeline;
  std::unique_ptr<geode::GeodeImagePipeline> imagePipeline;

  // --- Host-provided target texture (Phase 6 embedding) ---
  //
  // When non-null, `beginFrame()` renders into this texture instead of
  // creating its own offscreen target. The host retains ownership.
  wgpu::Texture hostTarget;

  // Texture format for all render targets. Matches the GeodeDevice's configured
  // format (RGBA8Unorm for headless, host-specified for embedded mode).
  wgpu::TextureFormat textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  // Per-frame resources, recreated in `beginFrame`.
  RenderViewport viewport;
  int pixelWidth = 0;
  int pixelHeight = 0;
  // Dimensions of the `target` / `msaaTarget` textures as they were
  // created. When the next `beginFrame` comes in at the same size,
  // the textures are reused (design doc 0030 Milestone 4.1); otherwise
  // they're reallocated.
  int targetWidth = 0;
  int targetHeight = 0;
  wgpu::Texture target;      // 1-sample resolve: RenderAttachment | CopySrc | TextureBinding.
  wgpu::Texture msaaTarget;  // 4× MSAA color attachment companion to `target`.

  // Single CommandEncoder owned by RendererGeode for the whole frame
  // (design doc 0030 Milestone 3). All `GeoEncoder` instances created
  // during the frame (base + push/pop layer / filter / mask) share
  // this CommandEncoder, so push/pop boundaries no longer force a
  // `queue().submit()`. Finalised + submitted exactly once in
  // `endFrame`.
  wgpu::CommandEncoder frameCommandEncoder;

  /// Finish + submit the current `frameCommandEncoder` and open a fresh
  /// one. Callers must have ended any open render pass (via
  /// `encoder->finish()`) before invoking this.
  ///
  /// Used when we need prior GPU work to be visible to a subsystem that
  /// uses its own CommandEncoder + submit (the filter engine). Incurs
  /// one extra submit per flush; most frames don't need it.
  void flushFrameCommandEncoder() {
    if (!frameCommandEncoder) {
      return;
    }
    wgpu::CommandBuffer cb = frameCommandEncoder.finish();
    device->queue().submit(1, &cb);
    device->countSubmit();
    wgpu::CommandEncoderDescriptor desc = {};
    desc.label = wgpuLabel("RendererGeodeFrameCE");
    frameCommandEncoder = device->device().createCommandEncoder(desc);
  }

  std::unique_ptr<geode::GeoEncoder> encoder;

  // Reusable scratch storage for gradient stop vectors — keeps the
  // per-fillPath allocation counts down and lets the `std::span` in
  // `LinearGradientParams` remain stable across the call.
  std::vector<geode::LinearGradientParams::Stop> gradientStopScratch;

  // CPU-side state.
  PaintParams paint;
  Transform2d deviceFromLocalTransform;
  std::vector<Transform2d> deviceFromLocalTransformStack;

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
    wgpu::Texture savedMsaaTarget;
    Transform2d savedDeviceFromLocalTransform;
    std::vector<Transform2d> savedDeviceFromLocalTransformStack;
    int savedPixelWidth = 0;
    int savedPixelHeight = 0;

    // The pattern tile being recorded.
    Box2d tileRect;                 // In pattern space (topLeft at origin).
    Transform2d targetFromPattern;  // Transform used when the tile is sampled.
    wgpu::Texture tileTexture;      // Offscreen tile (1-sample resolve) being sampled later.
    wgpu::Texture tileMsaaTexture;  // 4× MSAA companion used during tile recording.
    int tilePixelWidth = 0;
    int tilePixelHeight = 0;
    // Scale factor applied to all `setTransform` calls while this frame is
    // active, mapping pattern-tile units to tile-texture pixels so the
    // encoder's viewport math works out.
    Vector2d rasterScale = Vector2d(1.0, 1.0);
  };
  std::vector<PatternStackFrame> patternStack;

  // --------------------------------------------------------------------
  // M4.2 transient-texture pool (design doc 0030 §M4.2).
  //
  // Every push/pop of isolated-layer / filter-layer / mask / clip-mask
  // scratch allocates a resolve + MSAA-companion pair — prior to this
  // pool those allocations fired on every frame even when the same
  // document was re-rendered at the same viewport. The pool holds
  // released textures keyed by `(width, height, format, sampleCount,
  // usage)`; same-dim / same-format acquisition on a later frame pops
  // from the bucket instead of calling `createTexture`.
  //
  // Exact-size pooling (no power-of-two bucketing). Works for the
  // repeat-render case this PR targets because layer sizes are
  // derived from `pixelWidth`/`pixelHeight`, which don't change
  // between idle re-renders. A size-bucketing extension is a future
  // follow-up for viewport-resize scenarios.
  // --------------------------------------------------------------------

  /// Key used for texture-pool bucket lookup. Two textures are
  /// interchangeable iff every field matches — same size, same
  /// format, same MSAA sample count, same usage flags.
  struct TextureKey {
    uint32_t width = 0;
    uint32_t height = 0;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    uint32_t sampleCount = 1;
    wgpu::TextureUsage usage = wgpu::TextureUsage::None;

    auto operator<=>(const TextureKey& other) const = default;

    static TextureKey From(const wgpu::TextureDescriptor& desc) {
      return TextureKey{desc.size.width, desc.size.height, desc.format, desc.sampleCount,
                        desc.usage};
    }
  };
  struct TextureBucket {
    std::vector<wgpu::Texture> free;
    /// Monotonic frame index when this bucket was last touched by
    /// either `acquireTexture` or `releaseTexture`. Used by
    /// `evictStalePoolBuckets` to age out buckets whose size hasn't
    /// been seen in a while (viewport-resize scenarios).
    uint64_t lastUsedFrame = 0;
  };
  std::map<TextureKey, TextureBucket> texturePool;

  /// Per-bucket hard cap. Prevents a single size from accumulating
  /// unbounded textures even if a pathological frame pushes and pops
  /// dozens of layers at that size without ever reacquiring.
  static constexpr std::size_t kMaxPoolEntriesPerKey = 8;

  /// Drop a bucket entirely if it hasn't been touched in this many
  /// consecutive frames. At 60 fps this is ~2 seconds of idleness;
  /// long enough to survive transient dips (e.g. an editor dragging
  /// slightly then stopping) while still releasing memory on real
  /// viewport-size changes.
  static constexpr uint64_t kBucketEvictAfterFrames = 120;

  /// Monotonic frame counter. Incremented in `beginFrame`; used to
  /// stamp `TextureBucket::lastUsedFrame`.
  uint64_t currentFrameIndex = 0;

  /// Acquire a pooled texture matching `desc`, or create a fresh one
  /// on miss. Always increments the `textureCreates` counter on miss;
  /// never on hit. Returns a null texture on device failure.
  wgpu::Texture acquireTexture(const wgpu::TextureDescriptor& desc) {
    TextureBucket& bucket = texturePool[TextureKey::From(desc)];
    bucket.lastUsedFrame = currentFrameIndex;
    if (!bucket.free.empty()) {
      wgpu::Texture texture = std::move(bucket.free.back());
      bucket.free.pop_back();
      return texture;
    }
    wgpu::Texture texture = device->device().createTexture(desc);
    if (texture) {
      device->countTexture();
    }
    return texture;
  }

  /// Return a texture to its pool bucket IMMEDIATELY. Caller must
  /// pass the same descriptor used to acquire, otherwise the next
  /// acquire with the original descriptor will miss the bucket.
  ///
  /// Prefer `releaseTextureAtFrameEnd` for textures whose GPU work was
  /// recorded into the shared `frameCommandEncoder`: releasing those
  /// mid-frame would let a subsequent `acquireTexture` on the same
  /// bucket hand the texture back out before the GPU has finished
  /// writing it.
  void releaseTexture(wgpu::Texture texture, const wgpu::TextureDescriptor& desc) {
    if (!texture) {
      return;
    }
    TextureBucket& bucket = texturePool[TextureKey::From(desc)];
    bucket.lastUsedFrame = currentFrameIndex;
    if (bucket.free.size() >= kMaxPoolEntriesPerKey) {
      // Bucket full — let the released texture go out of scope instead
      // of unbounded growth.
      return;
    }
    bucket.free.push_back(std::move(texture));
  }

  /// Drop every bucket whose `lastUsedFrame` is older than
  /// `kBucketEvictAfterFrames` frames. Called at `beginFrame`. On
  /// steady-state workloads this is a no-op (all buckets refresh
  /// their stamps each frame); under viewport-resize churn it caps
  /// pool memory at whatever sizes have been seen recently.
  void evictStalePoolBuckets() {
    for (auto it = texturePool.begin(); it != texturePool.end();) {
      if (currentFrameIndex - it->second.lastUsedFrame > kBucketEvictAfterFrames) {
        it = texturePool.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Defer a release until after the frame's command buffer has been
  /// submitted. Used by `popIsolatedLayer` / `popFilterLayer` / etc.,
  /// where the layer texture is still referenced by commands recorded
  /// into the frame encoder and must not be recycled mid-frame.
  struct PendingRelease {
    wgpu::Texture texture;
    wgpu::TextureDescriptor desc;
  };
  std::vector<PendingRelease> framePendingReleases;

  void releaseTextureAtFrameEnd(wgpu::Texture texture, const wgpu::TextureDescriptor& desc) {
    if (!texture) {
      return;
    }
    framePendingReleases.push_back({std::move(texture), desc});
  }

  void drainPendingReleases() {
    for (auto& pending : framePendingReleases) {
      releaseTexture(std::move(pending.texture), pending.desc);
    }
    framePendingReleases.clear();
  }

  /// A saved encoder + target state for an in-progress isolated layer
  /// (`pushIsolatedLayer` / `popIsolatedLayer`). When the driver begins a
  /// group with non-identity opacity or a non-Normal blend mode, we
  /// redirect subsequent draws into an offscreen texture of the same size
  /// as the current target. On `pop`, the offscreen is composited back
  /// onto the saved target with the stored opacity.
  struct LayerStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;       // Outer 1-sample resolve target.
    wgpu::Texture savedMsaaTarget;   // Outer 4× MSAA color attachment.
    wgpu::Texture layerTexture;      // Inner layer 1-sample resolve.
    wgpu::Texture layerMsaaTexture;  // Inner layer 4× MSAA color attachment.
    /// Descriptors captured at push time so `popIsolatedLayer` can
    /// release the textures back to the correct pool bucket via
    /// `releaseTexture`.
    wgpu::TextureDescriptor layerDesc = {};
    wgpu::TextureDescriptor layerMsaaDesc = {};
    double opacity = 1.0;
    /// Phase 3d: SVG `mix-blend-mode`. `Normal` (default) keeps the
    /// plain premultiplied source-over compositing path;
    /// anything else drives `popIsolatedLayer` through the blend-blit
    /// variant that snapshots the parent and uses the W3C formulas.
    MixBlendMode blendMode = MixBlendMode::Normal;
  };
  std::vector<LayerStackFrame> layerStack;

  /// Phase 7: GPU filter-graph executor (owns compute pipelines).
  std::unique_ptr<geode::GeodeFilterEngine> filterEngine;

  /// Phase 3c: state for an in-progress `<mask>` element. Two offscreen
  /// texture pairs, one capturing the mask element's content and one
  /// capturing the masked subtree. `popMask` composites them via
  /// `GeoEncoder::blitFullTargetMasked` back onto the saved parent
  /// target.
  ///
  /// Phase sequencing matches `RendererTinySkia`:
  ///   * `pushMask` → allocate mask capture pair, redirect encoder.
  ///   * `transitionMaskToContent` → switch to content pair.
  ///   * `popMask` → blit (content * luminance(mask)) onto parent.
  struct MaskStackFrame {
    enum class Phase { Capturing, Content };
    Phase phase = Phase::Capturing;
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    wgpu::Texture savedMsaaTarget;
    wgpu::Texture maskTexture;  // Mask element's content (RGBA).
    wgpu::Texture maskMsaaTexture;
    wgpu::Texture contentTexture;  // Masked element's content (RGBA).
    wgpu::Texture contentMsaaTexture;
    /// Descriptors captured at push for M4.2 pool release.
    wgpu::TextureDescriptor maskDesc = {};
    wgpu::TextureDescriptor maskMsaaDesc = {};
    wgpu::TextureDescriptor contentDesc = {};
    wgpu::TextureDescriptor contentMsaaDesc = {};
    /// Raw mask-bounds rectangle from the driver, in the coordinate
    /// space of `maskBoundsTransform` (userSpaceOnUse or the
    /// objectBoundingBox-mapped user space — either way, NOT yet in
    /// device pixels).
    std::optional<Box2d> maskBounds;
    /// `deviceFromLocalTransform` snapshotted at `pushMask` time so that
    /// `popMask` can lift `maskBounds` into device-pixel space. This
    /// mirrors `RendererTinySkia::SurfaceFrame::maskBoundsTransform`.
    Transform2d maskBoundsTransform;
  };
  std::vector<MaskStackFrame> maskStack;

  /// A completed pattern tile ready to be sampled as fill or stroke paint.
  struct PatternPaintSlot {
    wgpu::Texture tile;
    Vector2d tileSize;              // In pattern space.
    Transform2d targetFromPattern;  // destFromSource naming.
    // `deviceFromLocalTransform` snapshotted at the time the outer element kicked
    // off `beginPatternTile`. This is the path→device transform that the
    // SAME outer element will use when its fill draw happens. Used at
    // pattern-paint build time to strip the canvas-scale / parent-transform
    // chain back out of the live `deviceFromLocalTransform`, so the resulting
    // `patternFromPath` matrix compares path-space positions against the
    // pattern tile's user-space coordinate system instead of accidentally
    // multiplying them by the viewBox→canvas scale.
    Transform2d deviceFromPathAtCapture;
  };
  std::optional<PatternPaintSlot> patternFillPaint;
  std::optional<PatternPaintSlot> patternStrokePaint;

  /// Axis-aligned clip rectangles in target-pixel coords. Each entry
  /// corresponds to a `pushClip` with a non-empty `clipRect`. The active
  /// scissor is the intersection of every entry's `pixelRect` on this
  /// stack.
  /// Entries with `valid == false` represent pushClip calls that had no
  /// `clipRect` component (path- or mask-only clips) — they're tracked
  /// so `popClip` stays balanced with `pushClip`.
  ///
  /// For non-axis-aligned ancestor transforms (e.g., a rotated `<svg>`
  /// or `<use>`), the scissor rect is the AABB of the transformed clip
  /// rect — which over-reports coverage. In that case the entry also
  /// carries the 4 polygon corners of the clip in device-pixel space,
  /// and the fragment shader tests each sample against the polygon's
  /// half-planes on top of the scissor rect. We only honour the TOPMOST
  /// polygon-bearing entry (`setClipPolygon` has no in-shader
  /// intersection with a previous polygon) — nested rotated clips are
  /// rare enough that we accept the over-coverage fallback.
  struct ClipStackEntry {
    Box2d pixelRect;
    bool valid = false;
    bool hasPolygon = false;
    Vector2d polygonCorners[4];
    /// Phase 3b path-clip mask. When non-null, `maskResolveView`
    /// references a 1-sample R8Unorm texture sampled by the fill /
    /// gradient pipelines through their clip-mask bindings. The
    /// texture is allocated per `pushClip` call — the Impl owns the
    /// wgpu::Texture to keep the resolve alive until `popClip`.
    ///
    /// For nested `<clipPath>` references, the pushClip code builds
    /// one mask per clip-path layer (deepest first); each outer
    /// layer's mask is rendered with the previous layer's mask as an
    /// input clip mask so every outer shape is intersected with the
    /// deeper union. The final (outermost) layer's resolve lives in
    /// `maskResolveView`; the intermediate layer textures are parked
    /// in `maskLayerTextures` so their wgpu::Texture ownership
    /// persists until `popClip`.
    wgpu::Texture maskMsaaTexture;
    wgpu::Texture maskResolveTexture;
    wgpu::TextureView maskResolveView;
    /// Paired (texture, descriptor) entries. Every clip-mask texture
    /// allocated by `pushClip` (across all nested layers) lives here
    /// until `popClip` hands them back to the M4.2 texture pool.
    std::vector<PendingRelease> maskLayerTextures;
  };

  /// Phase 7: state for an in-progress filter layer. Captures all draws
  /// between `pushFilterLayer` / `popFilterLayer` into an offscreen texture,
  /// then runs the stored `FilterGraph` through `GeodeFilterEngine` and
  /// composites the result back onto the outer target.
  struct FilterStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;       // Outer 1-sample resolve target.
    wgpu::Texture savedMsaaTarget;   // Outer 4× MSAA color attachment.
    wgpu::Texture layerTexture;      // Inner layer 1-sample resolve.
    wgpu::Texture layerMsaaTexture;  // Inner layer 4× MSAA color attachment.
    /// Descriptors captured at push for M4.2 pool release.
    wgpu::TextureDescriptor layerDesc = {};
    wgpu::TextureDescriptor layerMsaaDesc = {};
    components::FilterGraph filterGraph;
    Box2d filterRegion;
    Transform2d deviceFromFilter;  // Full CTM at push time.
    int filterBufferOffsetX = 0;   // Expansion into negative device X.
    int filterBufferOffsetY = 0;   // Expansion into negative device Y.
    std::vector<ClipStackEntry> savedClipStack;
  };
  std::vector<ClipStackEntry> clipStack;
  std::vector<FilterStackFrame> filterStack;

  /// Recompute the intersection of every rectangular clip entry on
  /// `clipStack` and apply it to the active encoder as a scissor,
  /// plus forward the topmost polygon clip (if any) through
  /// `setClipPolygon`. Called whenever the clip stack changes.
  void updateEncoderScissor() {
    if (!encoder) {
      return;
    }
    std::optional<Box2d> active;
    const ClipStackEntry* polygonEntry = nullptr;
    const ClipStackEntry* maskEntry = nullptr;
    for (const ClipStackEntry& entry : clipStack) {
      if (entry.valid) {
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
      if (entry.hasPolygon) {
        // Overwrite with the topmost polygon entry; no multi-polygon
        // intersection in the shader (see ClipStackEntry docs).
        polygonEntry = &entry;
      }
      if (entry.maskResolveView) {
        // Same deal for the path-clip mask — we always bind the
        // topmost one, and nested path-clip intersections are a
        // TODO (would need multiple clip-mask bindings in the
        // fragment shader or a per-clip compositing pass).
        maskEntry = &entry;
      }
    }

    if (polygonEntry != nullptr) {
      encoder->setClipPolygon(polygonEntry->polygonCorners);
    } else {
      encoder->clearClipPolygon();
    }

    if (maskEntry != nullptr) {
      // Pass the parent texture alongside the view so the encoder keeps
      // the Vulkan resource alive even after this clip-stack entry is
      // destroyed. The 1-arg setClipMask overload accidentally left the
      // view dangling across `popClip`→`pop_back`→destructor →
      // `updateEncoderScissor` sequences; see issue #551.
      encoder->setClipMask(maskEntry->maskResolveTexture, maskEntry->maskResolveView);
    } else {
      encoder->clearClipMask();
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
    const int32_t w = std::max(0, static_cast<int32_t>(std::ceil(active->bottomRight.x)) - x);
    const int32_t h = std::max(0, static_cast<int32_t>(std::ceil(active->bottomRight.y)) - y);
    encoder->setScissorRect(x, y, w, h);
  }

  // Stub-state latches — set on first warning in verbose mode so each
  // unimplemented feature logs exactly once per renderer.
  bool warnedLayer = false;
  bool warnedGradient = false;
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
  /// in PATH space (pre-MVP, pre-`deviceFromLocalTransform`). The pattern fragment
  /// shader needs pattern-tile-space coordinates. The driver gave us
  /// `targetFromPattern` in USER space (i.e., the viewBox frame the outer
  /// element was drawn in) — *not* device space — which is a semantic
  /// mismatch with the renderer, where `deviceFromLocalTransform` goes all the way
  /// from path space to device pixels (i.e., it bakes in the
  /// viewBox→canvas scale on top of the entity's own transform).
  ///
  /// To bridge the mismatch we capture `deviceFromPathAtCapture = deviceFromLocalTransform`
  /// at `beginPatternTile` time. Both the pattern's content subtree and the
  /// eventual fill draw use the same referencing element, so that transform
  /// is the path→device mapping we'd want for BOTH the tile raster and
  /// the final sample. The chain is:
  ///
  ///   pattern_pos  =  inverse(deviceFromPath_at_capture * targetFromPattern)
  ///                 · deviceFromLocalTransform · path_pos
  ///
  /// Expanding: the two `deviceFromLocalTransform`/`deviceFromPath_at_capture` matrices
  /// cancel (they're the same transform when the outer element is drawing
  /// its own fill immediately after the pattern subtree returns), leaving
  /// `inverse(targetFromPattern) · path_pos` — which sits in the user-space
  /// frame that `tileSize` is expressed in. That keeps the shader's
  /// `fract(patternPos / tileSize)` well-defined regardless of the
  /// viewBox→canvas scale.
  geode::GeoEncoder::PatternPaint buildPatternPaint(const PatternPaintSlot& slot,
                                                    double opacity) const {
    const Transform2d deviceFromPattern = slot.deviceFromPathAtCapture * slot.targetFromPattern;
    const Transform2d patternFromDevice = deviceFromPattern.inverse();
    const Transform2d patternFromPath = patternFromDevice * deviceFromLocalTransform;
    geode::GeoEncoder::PatternPaint p;
    p.tile = slot.tile;
    p.tileSize = slot.tileSize;
    p.patternFromPath = patternFromPath;
    p.opacity = opacity;
    return p;
  }

  /// Push the renderer's deviceFromLocalTransform onto the encoder before drawing.
  void syncTransform() {
    if (encoder) {
      encoder->setTransform(deviceFromLocalTransform);
    }
  }

  // --------------------------------------------------------------------
  // M2 path-encode cache (design doc 0030 §Milestone 2).
  //
  // Our entt `on_update<ComputedPathComponent>` / `on_destroy<ComputedPathComponent>`
  // listener is connected lazily at `draw()` entry. Presence is tracked
  // via a sentinel context component on the registry itself
  // (`ListenerInstalled`) — pointer-identity on `&registry` would be
  // unsafe across document lifetimes (a destroyed document's registry
  // memory can be reused, giving us the same pointer value for an
  // entirely different `entt::basic_registry` with no listener).
  // --------------------------------------------------------------------

  /// Sentinel context component, emplaced on a registry the first
  /// time it's seen by `ensureCacheInvalidationWired`. Lifetime ties
  /// to the registry — dies with it, so a re-allocated registry at
  /// the same address doesn't carry the tag.
  struct ListenerInstalled {};

  /// M6-B detection (design doc 0030 §M6 Bullet 2): track the source
  /// entity of the most recent `drawPath` call so `drawPath` can bump
  /// `sameSourceDrawPairs` whenever it sees two consecutive
  /// entity-matched calls. Reset to `entt::null` at `beginFrame`.
  /// Value is the `PathShape::sourceEntity`'s entity (not its
  /// registry-qualified handle — two drawPath calls from different
  /// registries are never considered "consecutive same-source").
  Entity lastDrawSourceEntity = entt::null;

  /// M6-B step 3 (design doc 0030 §M6 Bullet 2): deferred batch for
  /// consecutive `drawPath` calls that share a source entity + resolved
  /// solid paint + no stroke + no subtree complication. Each matching
  /// call appends its `deviceFromLocalTransform` into `transforms`; the batch
  /// is flushed by `flushPendingBatch()` whenever state changes in a
  /// way that invalidates the batch (paint-key mismatch on the next
  /// drawPath, any push/pop, end of frame, …). A flush of size >= 2
  /// routes through `GeoEncoder::fillPathInstanced` — one GPU draw
  /// with `instanceCount == N`. Size-1 flushes degrade to the regular
  /// single-draw path so we don't pay the per-instance-buffer cost for
  /// unbatched draws.
  struct PendingBatch {
    Entity sourceEntity = entt::null;
    css::RGBA color;
    FillRule rule = FillRule::NonZero;
    const geode::EncodedPath* encoded = nullptr;
    /// Reference to the source Path. Caller guarantees lifetime —
    /// the Path is stored on `ComputedPathComponent` (pinned by
    /// `GeodePathCacheComponent`'s cache invariant for the frame's
    /// lifetime).
    const Path* path = nullptr;
    /// `deviceFromLocalTransform` captured at each `drawPath`. On flush, the
    /// outer encoder transform is set to identity and these are
    /// uploaded as per-instance transforms (see
    /// `flushPendingBatch` for the math).
    std::vector<Transform2d> deviceFromLocalTransforms;
  };
  std::optional<PendingBatch> pendingBatch;

  /// Connect (or rewire) our `on_update<ComputedPathComponent>` /
  /// `on_destroy<ComputedPathComponent>` listener onto `registry`.
  /// Called at the start of each `draw()`. Idempotent for the same
  /// registry. When switching registries (test fixtures reusing one
  /// renderer), disconnects from the old first.
  void ensureCacheInvalidationWired(Registry& registry);

  /// Wipe the cache component from an entity when its source
  /// `ComputedPathComponent` is rewritten by `ShapeSystem` or destroyed.
  /// Connected to entt's `on_update` / `on_destroy` signals.
  /// File-scope free function with this signature is the only shape
  /// entt's `.connect<&fn>()` accepts that doesn't couple lifetime to
  /// `this` — see M2 notes in design doc 0030.
  static void onComputedPathChanged(Registry& registry, Entity entity);

  /// Scratch buffer for the no-source-entity stroke path. `getStrokeDerived`
  /// uses this as stable storage when there's no `GeodePathCacheComponent`
  /// to live on (e.g. `drawRect` / `drawEllipse` convenience draws). Only
  /// one active draw at a time, so a single slot is safe.
  Path strokeScratchPath;

  /// Value returned by `getFillEncode` / `getStrokeDerived` describing
  /// which encode the caller should pass down to `GeoEncoder`.
  struct StrokeDerived {
    /// Path to draw. Null means "no stroke geometry — skip the draw".
    /// Points into the entity's cache slot on hit, or into
    /// `strokeScratchPath` on the no-entity fallback.
    const Path* strokedPath = nullptr;
    /// Precomputed encode pointer for `GeoEncoder`. Non-null only when
    /// the stroke came from a cache slot — the no-entity fallback
    /// leaves this null and lets `GeoEncoder` encode inline.
    const geode::EncodedPath* encoded = nullptr;
    /// Fill rule to use. For open-path strokes, `strokeToFill` emits one
    /// subpath → NonZero; for closed-path strokes, two → EvenOdd.
    FillRule fillRule = FillRule::NonZero;
  };

  /// Encode-side of the cache. If `source` holds a valid entity handle,
  /// installs / reuses a `GeodePathCacheComponent::fillEncode` on it and
  /// returns a stable pointer into that component. If `source` is null
  /// (non-driver callers), returns null and `GeoEncoder` will encode
  /// inline — the old pre-M2 code path.
  const geode::EncodedPath* getFillEncode(EntityHandle source, const Path& path, FillRule rule) {
    if (!source) {
      return nullptr;
    }
    auto& cache = source.get_or_emplace<geode::GeodePathCacheComponent>();
    if (!cache.fillEncode) {
      device->countPathEncode();
      cache.fillEncode = geode::GeodePathEncoder::encode(path, rule);
    }
    return &*cache.fillEncode;
  }

  /// Pack a 2D affine into the 8-float wire format the shader expects
  /// (two `vec4f` rows, `(a, c, e, 0)` / `(b, d, f, 0)` — see
  /// `struct InstanceTransform` in `shaders/slug_fill.wgsl`).
  /// `Transform2d::data` is column-major `[a, b, c, d, e, f]`.
  static void packTransform(const Transform2d& xf, float out[8]) {
    out[0] = static_cast<float>(xf.data[0]);  // a
    out[1] = static_cast<float>(xf.data[2]);  // c
    out[2] = static_cast<float>(xf.data[4]);  // e
    out[3] = 0.0f;
    out[4] = static_cast<float>(xf.data[1]);  // b
    out[5] = static_cast<float>(xf.data[3]);  // d
    out[6] = static_cast<float>(xf.data[5]);  // f
    out[7] = 0.0f;
  }

  /// Emit any pending M6-B batch. No-op if there's nothing pending.
  /// On size == 1 the batch degrades to a single `fillPath` call so we
  /// don't pay the per-instance-buffer cost when we accumulated
  /// exactly one draw. Size >= 2 is one instanced GPU draw.
  ///
  /// Flushing mutates `deviceFromLocalTransform` to push the batch's
  /// transform(s) down to the encoder, then RESTORES it to the
  /// caller's current value. Without the restore, a flush in the
  /// middle of `drawPath` (between fill and stroke, for example)
  /// would leave the transform stuck at the flushed batch's value
  /// for the subsequent stroke emit — breaks any fixture that
  /// mixes batchable fills with stroked siblings.
  void flushPendingBatch() {
    if (!pendingBatch.has_value() || pendingBatch->deviceFromLocalTransforms.empty()) {
      pendingBatch.reset();
      return;
    }
    if (!encoder) {
      pendingBatch.reset();
      return;
    }
    const Transform2d savedDeviceFromLocalTransform = deviceFromLocalTransform;
    PendingBatch batch = std::move(*pendingBatch);
    pendingBatch.reset();

    if (batch.deviceFromLocalTransforms.size() == 1) {
      // Single draw — restore the captured transform + use the
      // non-instanced path.
      deviceFromLocalTransform = batch.deviceFromLocalTransforms.front();
      syncTransform();
      encoder->fillPath(*batch.path, batch.color, batch.rule, batch.encoded);
    } else {
      // Instanced: set encoder transform to identity so the shader's
      // `uniforms.mvp` carries only the orthographic screen-pixel mapping.
      // Each instance transform already encodes the full
      // `worldFromEntity * surfaceFromCanvas` composition that a
      // non-batched draw would fold into deviceFromLocalTransform; compose with
      // identity `uniforms.mvp` is equivalent to composing with the
      // original deviceFromLocalTransform per-draw.
      deviceFromLocalTransform = Transform2d();
      syncTransform();

      // Pack transforms into the wire format the shader expects.
      std::vector<float> packed(batch.deviceFromLocalTransforms.size() * 8u);
      for (size_t i = 0; i < batch.deviceFromLocalTransforms.size(); ++i) {
        packTransform(batch.deviceFromLocalTransforms[i], packed.data() + i * 8u);
      }
      encoder->fillPathInstanced(*batch.encoded, batch.color, batch.rule, packed);
    }

    // Restore so subsequent draw/state ops see the driver-set
    // transform intact. Draw-emitting helpers (`syncTransform` +
    // `encoder->fillPath*`) read `deviceFromLocalTransform` when re-entered,
    // so we don't need to re-sync the encoder right now.
    deviceFromLocalTransform = savedDeviceFromLocalTransform;
  }

  /// Predicate: would a batchable draw with this key extend the
  /// currently pending batch, or does it start a new one? Returns
  /// true when the current `drawPath` call should NOT emit (because
  /// it's been absorbed into a batch). Always returns true on a
  /// non-empty batch state — either appends or flushes + starts new.
  /// The caller is expected to have already verified the draw is
  /// "batch-compatible" (solid paint, no stroke, has source entity,
  /// has cached fill encode, no in-flight pattern).
  bool tryAppendOrStartBatch(Entity sourceEntity, const Path& path, const css::RGBA& color,
                             FillRule rule, const geode::EncodedPath* encoded) {
    const bool matches = pendingBatch.has_value() && pendingBatch->sourceEntity == sourceEntity &&
                         pendingBatch->color == color && pendingBatch->rule == rule;
    if (matches) {
      pendingBatch->deviceFromLocalTransforms.push_back(deviceFromLocalTransform);
      return true;
    }
    // Key doesn't match — flush whatever's pending, then start fresh.
    flushPendingBatch();
    pendingBatch = PendingBatch{};
    pendingBatch->sourceEntity = sourceEntity;
    pendingBatch->color = color;
    pendingBatch->rule = rule;
    pendingBatch->encoded = encoded;
    pendingBatch->path = &path;
    pendingBatch->deviceFromLocalTransforms.push_back(deviceFromLocalTransform);
    return true;
  }

  /// Determine the fill rule for a stroked outline per the subpath-count
  /// rule documented in `RendererGeode::drawPath`. Open-path strokes
  /// produce one subpath (NonZero); closed-path strokes produce two
  /// same-winding subpaths (EvenOdd hollow-ring semantics).
  static FillRule strokeFillRuleFor(const Path& strokedOutline) {
    size_t subpathCount = 0;
    for (const auto& cmd : strokedOutline.commands()) {
      if (cmd.verb == Path::Verb::MoveTo) {
        ++subpathCount;
      }
    }
    return (subpathCount <= 1) ? FillRule::NonZero : FillRule::EvenOdd;
  }

  /// Stroke-side of the cache. Builds (or reuses) the `strokeToFill`
  /// output and its encode on `source`'s `GeodePathCacheComponent`.
  /// Returns a `StrokeDerived` pointing into the cache (entity path) or
  /// into `strokeScratchPath` (no-entity fallback). The caller must
  /// check `strokedPath == nullptr` for the "zero-stroke" case.
  StrokeDerived getStrokeDerived(EntityHandle source, const Path& geometry,
                                 const StrokeStyle& strokeStyle) {
    StrokeDerived result;
    if (source) {
      auto& cache = source.get_or_emplace<geode::GeodePathCacheComponent>();
      if (!cache.strokeSlot || cache.strokeSlot->strokeKey != strokeStyle) {
        // Miss (or stroke-params changed) — rebuild.
        Path stroked = geometry.strokeToFill(strokeStyle);
        if (stroked.empty()) {
          cache.strokeSlot.reset();
          return result;  // strokedPath stays null.
        }
        const FillRule fillRule = strokeFillRuleFor(stroked);
        device->countPathEncode();
        geode::EncodedPath encoded = geode::GeodePathEncoder::encode(stroked, fillRule);
        // GCC 14 libstdc++ rejects `.emplace()` here with "is_constructible_v<StrokeSlot> was not
        // satisfied"; clang + libc++ accepts it. Build the value explicitly and assign to sidestep
        // the toolchain disagreement.
        cache.strokeSlot = geode::GeodePathCacheComponent::StrokeSlot{
            .strokeKey = strokeStyle,
            .strokedPath = std::move(stroked),
            .strokedEncode = std::move(encoded),
            .strokeFillRule = fillRule,
        };
      }
      result.strokedPath = &cache.strokeSlot->strokedPath;
      result.encoded = &cache.strokeSlot->strokedEncode;
      result.fillRule = cache.strokeSlot->strokeFillRule;
      return result;
    }
    // No-entity fallback: compute into the Impl-local scratch buffer.
    // GeoEncoder will encode inline when `encoded` is left null.
    strokeScratchPath = geometry.strokeToFill(strokeStyle);
    if (strokeScratchPath.empty()) {
      return result;
    }
    result.strokedPath = &strokeScratchPath;
    result.fillRule = strokeFillRuleFor(strokeScratchPath);
    return result;
  }

  /// Issue a fill of the given path using the current `paint.fill`. Handles
  /// solid colors, linear and radial gradients, and pattern tiles. None and
  /// unsupported types are ignored or fall back to their fallback color.
  ///
  /// `precomputedEncoded` is the M2 cache-hit payload (see
  /// `getFillEncode`). When non-null, the encoder skips the
  /// `GeodePathEncoder::encode` + `countPathEncode()` pair; otherwise
  /// `GeoEncoder` runs the inline encode path.
  void fillResolved(const Path& path, FillRule rule,
                    const geode::EncodedPath* precomputedEncoded = nullptr) {
    if (!encoder) {
      return;
    }
    // Pattern dispatch comes first: a pattern slot is populated by the
    // driver via `endPatternTile`, and is consumed by the very next fill or
    // stroke draw (matching the CPU-renderer semantics).
    if (patternFillPaint.has_value()) {
      syncTransform();
      const double opacity = paint.fillOpacity;
      encoder->fillPathPattern(path, rule, buildPatternPaint(*patternFillPaint, opacity),
                               precomputedEncoded);
      patternFillPaint.reset();
      return;
    }
    const double effectiveOpacity = paint.fillOpacity;
    drawPaintedPath(path, paint.fill, effectiveOpacity, rule, precomputedEncoded);
  }

  /// Core dispatch: given a path and a resolved paint server, emit the
  /// appropriate fill call (solid color or gradient).
  void drawPaintedPath(const Path& path, const components::ResolvedPaintServer& server,
                       double effectiveOpacity, FillRule rule,
                       const geode::EncodedPath* precomputedEncoded = nullptr) {
    drawPaintedPathAgainst(path, path, server, effectiveOpacity, rule, precomputedEncoded);
  }

  /// Same as `drawPaintedPath`, but the gradient's objectBoundingBox is
  /// computed from `geometryPath` while the GPU draw uses `drawPath`. This
  /// is required for stroked outlines: SVG specifies that the
  /// `objectBoundingBox` of a stroke gradient is derived from the *original*
  /// geometry, not the expanded stroke outline, otherwise a thick stroke
  /// would warp the gradient direction relative to the underlying shape.
  void drawPaintedPathAgainst(const Path& geometryPath, const Path& drawPath,
                              const components::ResolvedPaintServer& server,
                              double effectiveOpacity, FillRule rule,
                              const geode::EncodedPath* precomputedEncoded = nullptr) {
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
      encoder->fillPath(drawPath, solid->color.resolve(currentColor, opacity), rule,
                        precomputedEncoded);
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
        encoder->fillPathLinearGradient(drawPath, *linear, rule, precomputedEncoded);
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
          encoder->fillPathRadialGradient(drawPath, *radial->gradient, rule, precomputedEncoded);
          return;
        }
        if (radial->solidFallback.has_value()) {
          // SVG2 degenerate radial (r=0): paint the last stop color as a
          // solid fill so the element remains visible.
          syncTransform();
          encoder->fillPath(drawPath, *radial->solidFallback, rule, precomputedEncoded);
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
        encoder->fillPath(drawPath, ref->fallback->resolve(currentColor, opacity), rule,
                          precomputedEncoded);
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

  // No custom destructor: the M2 cache-invalidation listener is a
  // free function with no dependency on `this`, so the natural entt
  // lifecycle is correct — connections die with the `Registry` they
  // live on. Calling `.disconnect<&fn>()` from a dtor would UB when
  // the registry is destroyed BEFORE the renderer (common in tests
  // where an `SVGDocument` is declared after its `Renderer` in the
  // same scope, so the document destructs first). Leaving the
  // connection attached is harmless: either the registry is alive
  // and a subsequent geometry change fires `remove<GeodePathCacheComponent>`
  // (which is a no-op if the component isn't present), or the
  // registry is gone and no signal will ever fire again.

  /// Initialize GPU pipelines after the device is set. Called from all
  /// RendererGeode constructors to avoid duplicating pipeline-creation.
  void initPipelines(bool verboseFlag) {
    const wgpu::TextureFormat fmt = device->textureFormat();
    textureFormat = fmt;
    device->setCounters(&counters);
    pipeline = std::make_unique<geode::GeodePipeline>(
        device->device(), fmt, device->useAlphaCoverageAA(), device->sampleCount());
    gradientPipeline = std::make_unique<geode::GeodeGradientPipeline>(
        device->device(), fmt, device->useAlphaCoverageAA(), device->sampleCount());
    imagePipeline =
        std::make_unique<geode::GeodeImagePipeline>(device->device(), fmt, device->sampleCount());
    filterEngine = std::make_unique<geode::GeodeFilterEngine>(*device, verboseFlag);
  }
};

void RendererGeode::Impl::ensureCacheInvalidationWired(Registry& registry) {
  // Sentinel lives on the registry's context store, so its presence
  // implies "this registry has already had our listener connected".
  // When the registry is destroyed the sentinel goes with it — a
  // later registry allocated at the same address will (correctly)
  // miss the sentinel and get its own listener. Pointer-identity on
  // `&registry` alone can't distinguish those cases.
  if (registry.ctx().contains<ListenerInstalled>()) {
    return;
  }
  registry.ctx().emplace<ListenerInstalled>();
  registry.on_update<components::ComputedPathComponent>().connect<&Impl::onComputedPathChanged>();
  registry.on_destroy<components::ComputedPathComponent>().connect<&Impl::onComputedPathChanged>();
  // Leaving the connection attached across renderer destruction is
  // intentional: our free-function listener has no `this`-capture,
  // so it's safe to outlive the renderer. Connections die with the
  // registry.
}

void RendererGeode::Impl::onComputedPathChanged(Registry& registry, Entity entity) {
  // entt allows `remove` on a component the entity doesn't hold — it's a
  // cheap no-op in that case. We don't need an `all_of` guard.
  registry.remove<geode::GeodePathCacheComponent>(entity);
}

RendererGeode::RendererGeode(bool verbose) : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  impl_->device = geode::GeodeDevice::CreateHeadless();
  if (!impl_->device) {
    if (verbose) {
      std::cerr << "RendererGeode: GeodeDevice::CreateHeadless() failed — entering no-op mode\n";
    }
    return;
  }
  impl_->initPipelines(verbose);
}

RendererGeode::RendererGeode(std::shared_ptr<geode::GeodeDevice> device, bool verbose)
    : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  impl_->device = std::move(device);
  if (!impl_->device) {
    if (verbose) {
      std::cerr << "RendererGeode: null GeodeDevice passed — entering no-op mode\n";
    }
    return;
  }
  impl_->initPipelines(verbose);
}

void RendererGeode::enableTimestamps(bool /*enabled*/) {
  // Reserved for future work (design doc 0030, "Future Work"). Counters
  // are the durable regression signal and are always enabled.
}

FrameTimings RendererGeode::lastFrameTimings() const {
  FrameTimings timings;
  timings.counters = impl_->counters;
  // `renderPassNs` / `totalGpuNs` stay zero until timestamp support lands.
  return timings;
}

void RendererGeode::setTargetTexture(wgpu::Texture texture) {
  impl_->hostTarget = std::move(texture);
}

void RendererGeode::clearTargetTexture() {
  impl_->hostTarget = wgpu::Texture();
}

RendererGeode::~RendererGeode() {
  // GeodeDevice holds a raw `counters_` pointer into our Impl (see `Impl::initPipelines` →
  // `device->setCounters(&counters)`). If this renderer's counters are still the ones the
  // (shared) device refers to, clear the pointer before our Impl (and its `counters` member) is
  // freed. Otherwise the next `countBuffer`/`countTexture` call by any peer renderer sharing this
  // device will dereference freed memory — which is exactly how chained feImage rendering
  // crashes: multiple offscreen renderers share one device, each one overrides `counters_` in
  // `initPipelines`, and the first one destroyed leaves `counters_` dangling for the others.
  if (impl_ && impl_->device && impl_->device->counters() == &impl_->counters) {
    impl_->device->setCounters(nullptr);
  }
}
RendererGeode::RendererGeode(RendererGeode&&) noexcept = default;
RendererGeode& RendererGeode::operator=(RendererGeode&&) noexcept = default;

void RendererGeode::draw(SVGDocument& document) {
  // Wire the M2 cache-invalidation listener onto this document's
  // registry BEFORE the driver runs `RenderingContext::instantiateRenderTree`.
  // The listener must be connected when `ShapeSystem` fires its
  // `on_update<ComputedPathComponent>` signals; otherwise a geometry
  // change between draws would silently leave a stale encode in
  // `GeodePathCacheComponent`.
  impl_->ensureCacheInvalidationWired(document.registry());

  RendererDriver driver(*this, impl_->verbose);
  driver.draw(document);
}

int RendererGeode::width() const {
  return impl_->pixelWidth;
}
int RendererGeode::height() const {
  return impl_->pixelHeight;
}

void RendererGeode::beginFrame(const RenderViewport& viewport) {
  // Drain deferred-destroy resources from the previous frame before allocating
  // new ones. By this point any GPU submission from the prior frame has had a
  // chance to finish, and WebGPU's internal ref-counting keeps resources alive
  // for any still-in-flight command buffers.
  if (impl_->device) {
    impl_->device->drainDeferredDestroys();
  }

  impl_->viewport = viewport;
  impl_->pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  impl_->pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);
  impl_->deviceFromLocalTransform = Transform2d();
  impl_->deviceFromLocalTransformStack.clear();
  impl_->paint = PaintParams();
  impl_->encoder.reset();

  // M4.2: stamp each frame with a monotonic index and age out pool
  // buckets that haven't been touched in the last
  // `kBucketEvictAfterFrames` frames. Caps memory under viewport
  // resize.
  ++impl_->currentFrameIndex;
  impl_->evictStalePoolBuckets();

  // M6-B detection: drop the previous-draw source-entity memo so
  // cross-frame draws don't show up as "same-source runs".
  impl_->lastDrawSourceEntity = entt::null;

  // Reset counters regardless of device state.
  impl_->counters.reset();

  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    impl_->target = wgpu::Texture();
    impl_->msaaTarget = wgpu::Texture();
    impl_->targetWidth = 0;
    impl_->targetHeight = 0;
    return;
  }

  // Wire the counters onto the device for this frame. A shared GeodeDevice
  // may have been handed to another renderer between frames — the last
  // caller wins, which is exactly the serial per-frame access pattern we
  // want. Must run AFTER the null-device guard above so headless systems
  // don't null-ptr-crash in draw()→beginFrame().
  impl_->device->setCounters(&impl_->counters);

  if (impl_->hostTarget) {
    // Embedded mode: render into the host-provided target texture.
    // Override pixel dimensions from the texture itself.
    impl_->pixelWidth = static_cast<int>(impl_->hostTarget.getWidth());
    impl_->pixelHeight = static_cast<int>(impl_->hostTarget.getHeight());
    impl_->target = impl_->hostTarget;
    impl_->targetWidth = 0;
    impl_->targetHeight = 0;

    // 4× MSAA color attachment (when sampleCount > 1).
    const uint32_t sc = impl_->device->sampleCount();
    if (sc > 1) {
      wgpu::TextureDescriptor msaaDesc = {};
      msaaDesc.label = wgpuLabel("RendererGeodeTargetMSAA");
      msaaDesc.size = {static_cast<uint32_t>(impl_->pixelWidth),
                       static_cast<uint32_t>(impl_->pixelHeight), 1};
      msaaDesc.format = impl_->textureFormat;
      msaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
      msaaDesc.mipLevelCount = 1;
      msaaDesc.sampleCount = sc;
      msaaDesc.dimension = wgpu::TextureDimension::_2D;
      impl_->msaaTarget = impl_->device->device().createTexture(msaaDesc);
      impl_->device->countTexture();
    } else {
      impl_->msaaTarget = wgpu::Texture();
    }
  } else {
    // Headless mode: reuse render targets across same-size frames (design doc
    // 0030 Milestone 4.1). Content is cleared by the encoder's first
    // render-pass `LoadOp::Clear`, so lingering pixels from the previous
    // frame don't leak into this one.
    const bool canReuseTargets = impl_->target && impl_->targetWidth == impl_->pixelWidth &&
                                 impl_->targetHeight == impl_->pixelHeight;

    if (!canReuseTargets) {
      // 1-sample resolve target: what `takeSnapshot()` copies back and what
      // pattern / layer blits sample from.
      wgpu::TextureDescriptor td = {};
      td.label = wgpuLabel("RendererGeodeTarget");
      td.size = {static_cast<uint32_t>(impl_->pixelWidth),
                 static_cast<uint32_t>(impl_->pixelHeight), 1};
      td.format = impl_->textureFormat;
      td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                 wgpu::TextureUsage::TextureBinding;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.dimension = wgpu::TextureDimension::_2D;
      impl_->target = impl_->device->device().createTexture(td);
      impl_->device->countTexture();

      // 4× MSAA color attachment (when sampleCount > 1): all draws land here,
      // hardware resolves to `target` at pass end. Skipped on the
      // alpha-coverage path (sampleCount == 1) where draws go directly
      // into `target` with no intermediate MSAA texture.
      const uint32_t sc = impl_->device->sampleCount();
      if (sc > 1) {
        wgpu::TextureDescriptor msaaDesc = {};
        msaaDesc.label = wgpuLabel("RendererGeodeTargetMSAA");
        msaaDesc.size = td.size;
        msaaDesc.format = impl_->textureFormat;
        msaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
        msaaDesc.mipLevelCount = 1;
        msaaDesc.sampleCount = sc;
        msaaDesc.dimension = wgpu::TextureDimension::_2D;
        impl_->msaaTarget = impl_->device->device().createTexture(msaaDesc);
        impl_->device->countTexture();
      } else {
        impl_->msaaTarget = wgpu::Texture();
      }
      impl_->targetWidth = impl_->pixelWidth;
      impl_->targetHeight = impl_->pixelHeight;
    }
  }

  // Single CommandEncoder for the entire frame — shared across the
  // base encoder and every push/pop layer/filter/mask helper. One
  // queue submit at `endFrame`. Design doc 0030 Milestone 3.
  wgpu::CommandEncoderDescriptor cedesc = {};
  cedesc.label = wgpuLabel("RendererGeodeFrameCE");
  impl_->frameCommandEncoder = impl_->device->device().createCommandEncoder(cedesc);

  impl_->encoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->msaaTarget, impl_->target, impl_->frameCommandEncoder);
  // Default to a transparent clear so an empty frame matches the other
  // backends' "no document content" appearance.
  impl_->encoder->clear(css::RGBA(0, 0, 0, 0));
}

void RendererGeode::endFrame() {
  // M6-B step 3: flush any pending `<use>`-batch before closing out
  // the frame. Without this, the last run of batchable draws in the
  // frame would never emit.
  impl_->flushPendingBatch();

  if (impl_->encoder) {
    // Ends the open render pass without submitting — shared-mode.
    impl_->encoder->finish();
    impl_->encoder.reset();
  }

  // Finalise and submit the single frame-wide CommandEncoder. After
  // this one submit, all recorded render passes (base + every pushed
  // layer / filter / mask) execute on the GPU in program order.
  if (impl_->frameCommandEncoder) {
    wgpu::CommandBuffer cmdBuf = impl_->frameCommandEncoder.finish();
    impl_->device->queue().submit(1, &cmdBuf);
    impl_->device->countSubmit();
    impl_->frameCommandEncoder = wgpu::CommandEncoder();
  }

  // Now that the command buffer is submitted, it's safe to return the
  // frame's transient layer / filter / mask / snapshot textures to
  // the pool. WebGPU's driver tracks texture dependencies across
  // submits, so acquiring these on the next frame will schedule the
  // new writes after the previous submit's GPU work completes.
  impl_->drainPendingReleases();

  impl_->deviceFromLocalTransform = Transform2d();
  impl_->deviceFromLocalTransformStack.clear();
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
    impl_->deviceFromLocalTransform = scaled;
    return;
  }
  if (!impl_->filterStack.empty()) {
    const auto& filterFrame = impl_->filterStack.back();
    if (filterFrame.filterBufferOffsetX != 0 || filterFrame.filterBufferOffsetY != 0) {
      impl_->deviceFromLocalTransform =
          transform *
          Transform2d::Translate(filterFrame.filterBufferOffsetX, filterFrame.filterBufferOffsetY);
      return;
    }
  }
  impl_->deviceFromLocalTransform = transform;
}

void RendererGeode::pushTransform(const Transform2d& transform) {
  impl_->deviceFromLocalTransformStack.push_back(impl_->deviceFromLocalTransform);
  impl_->deviceFromLocalTransform = transform * impl_->deviceFromLocalTransform;
}

void RendererGeode::popTransform() {
  if (impl_->deviceFromLocalTransformStack.empty()) {
    return;
  }
  impl_->deviceFromLocalTransform = impl_->deviceFromLocalTransformStack.back();
  impl_->deviceFromLocalTransformStack.pop_back();
}

void RendererGeode::pushClip(const ResolvedClip& clip) {
  // M6-B step 3: flush batch before a state change — a subsequent
  // drawPath inside the new clip is no longer "batch-compatible"
  // with the pending run from the outer clip region.
  impl_->flushPendingBatch();

  // Rectangular clip (the nested-`<svg>` viewport, `overflow: hidden`, and
  // `<image>` dest-rect cases) is implemented via the WebGPU scissor rect
  // (plus the Phase 3a polygon clip for non-axis-aligned ancestors).
  // Path-based clip-paths are implemented via the Phase 3b mask
  // pipeline below. `<mask>` alpha masks run through the Phase 3c mask
  // blit pipeline via `pushMaskLayer` — by the time `pushClip` runs the
  // mask is already composed upstream, so there's nothing to do for
  // `clip.mask` here.

  // Compose the incoming clip rect (in user-space) with the current
  // transform to get pixel-space coordinates, then push onto the stack.
  // The active scissor is the INTERSECTION of everything on the stack.
  Impl::ClipStackEntry entry;
  if (clip.clipRect.has_value()) {
    const Transform2d& t = impl_->deviceFromLocalTransform;
    entry.pixelRect = t.transformBox(*clip.clipRect);
    entry.valid = true;

    // Detect a non-axis-aligned ancestor transform — rotation or shear
    // means the true clip shape is a parallelogram, not a rectangle,
    // and a rectangular scissor can only describe its AABB. In that
    // case we carry the 4 transformed corners through to the encoder
    // as a polygon clip so the fragment shader can do a half-plane
    // test per sample.
    const double a = t.data[0];
    const double b = t.data[1];
    const double c = t.data[2];
    const double d = t.data[3];
    // Axis-aligned iff (no shear: b=c=0) OR (90°-rotation: a=d=0).
    // Use a small tolerance to absorb floating-point noise in the
    // composed transform chain.
    constexpr double kAxisAlignedEps = 1e-9;
    const bool axisAligned = (std::abs(b) < kAxisAlignedEps && std::abs(c) < kAxisAlignedEps) ||
                             (std::abs(a) < kAxisAlignedEps && std::abs(d) < kAxisAlignedEps);
    if (!axisAligned) {
      const Box2d& local = *clip.clipRect;
      const Vector2d tl(local.topLeft.x, local.topLeft.y);
      const Vector2d tr(local.bottomRight.x, local.topLeft.y);
      const Vector2d br(local.bottomRight.x, local.bottomRight.y);
      const Vector2d bl(local.topLeft.x, local.bottomRight.y);
      entry.polygonCorners[0] = t.transformPosition(tl);
      entry.polygonCorners[1] = t.transformPosition(tr);
      entry.polygonCorners[2] = t.transformPosition(br);
      entry.polygonCorners[3] = t.transformPosition(bl);
      entry.hasPolygon = true;
    }
  }

  // Phase 3b: path-clip mask. When the clip has any `clipPaths`,
  // render them into R8Unorm mask textures via the Slug mask
  // pipeline and hand the resolved view of the outermost layer to
  // the encoder so subsequent fill / gradient draws multiply clip
  // coverage into their output.
  //
  // `clip.clipPaths` is a flat list in traversal order with a
  // per-shape `layer` index. Paths at the same layer are UNIONED;
  // when the layer decreases (we cross back from a nested clipPath
  // reference to its parent), that whole nested layer is
  // INTERSECTED with each path at the outer layer. This matches the
  // recursive `buildLayerMask` in `RendererTinySkia::pushClip`.
  //
  // The union step lives in the hardware blend (`BlendOperation::Max`
  // on the R channel). The intersection step lives in the slug_mask
  // fragment shader: when a deeper layer's mask is bound as the
  // `clipMaskTexture`, each outer-layer shape samples it at its
  // pixel center and multiplies the coverage output, so
  // `max(shape_i ∩ nested)` = `(union of shape_i) ∩ nested`.
  //
  // Bottom-up traversal order: we scan the clipPaths list and
  // partition it into contiguous runs of equal layer. Deeper layers
  // appear AFTER outer layers in the list (the driver emits them in
  // the order they're encountered during DFS, and references push a
  // higher layer for the nested clip's children). So the deepest
  // layer is at the tail; we render each layer's mask in reverse,
  // binding the previously-rendered deeper mask as the input clip.
  if (!clip.clipPaths.empty() && impl_->device && impl_->encoder && impl_->pixelWidth > 0 &&
      impl_->pixelHeight > 0) {
    const wgpu::Device& dev = impl_->device->device();

    const auto makeResolveTexture = [&](const char* label, wgpu::TextureDescriptor& outDesc) {
      outDesc = wgpu::TextureDescriptor{};
      outDesc.label = wgpuLabel(label);
      outDesc.size = {static_cast<uint32_t>(impl_->pixelWidth),
                      static_cast<uint32_t>(impl_->pixelHeight), 1u};
      outDesc.format = wgpu::TextureFormat::RGBA8Unorm;
      outDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
      outDesc.mipLevelCount = 1;
      outDesc.sampleCount = 1;
      outDesc.dimension = wgpu::TextureDimension::_2D;
      return impl_->acquireTexture(outDesc);
    };
    const auto makeMsaaTexture = [&](const char* label,
                                     wgpu::TextureDescriptor& outDesc) -> wgpu::Texture {
      if (impl_->device->sampleCount() == 1) {
        // Alpha-coverage path: no separate MSAA texture needed.
        return {};
      }
      outDesc = wgpu::TextureDescriptor{};
      outDesc.label = wgpuLabel(label);
      outDesc.size = {static_cast<uint32_t>(impl_->pixelWidth),
                      static_cast<uint32_t>(impl_->pixelHeight), 1u};
      outDesc.format = wgpu::TextureFormat::RGBA8Unorm;
      outDesc.usage = wgpu::TextureUsage::RenderAttachment;
      outDesc.mipLevelCount = 1;
      outDesc.sampleCount = 4;
      outDesc.dimension = wgpu::TextureDimension::_2D;
      return impl_->acquireTexture(outDesc);
    };

    // Partition `clip.clipPaths` into contiguous [begin, end) ranges,
    // one per layer, in the order they appear. Layers appear in
    // traversal order — within a run the layer is constant, and
    // boundaries correspond to clipPath-reference crossings. For
    // each run we'll render one mask texture.
    struct LayerRun {
      size_t begin;
      size_t end;
      int layer;
    };
    std::vector<LayerRun> runs;
    {
      size_t i = 0;
      while (i < clip.clipPaths.size()) {
        const int layer = clip.clipPaths[i].layer;
        size_t j = i + 1;
        while (j < clip.clipPaths.size() && clip.clipPaths[j].layer == layer) {
          ++j;
        }
        runs.push_back({i, j, layer});
        i = j;
      }
    }

    // Render runs bottom-up (deepest layer first). Each run's mask
    // is rendered with the previously-rendered deeper mask bound as
    // the input clip so shapes get intersected with the deeper
    // union. Runs at the same layer don't intersect with each other
    // — the Max blend on the R channel handles union within a run.
    const Transform2d savedDeviceFromLocalTransform = impl_->deviceFromLocalTransform;

    // If any clip stack entry already carries a path mask (e.g., an
    // ancestor `<g>` with its own `clip-path`), use the topmost one
    // as the initial nested mask so this new clip gets intersected
    // with it as it's being rendered. Without this seed the outer
    // ancestor clip would be lost the moment the inner clip lands
    // because `updateEncoderScissor` only binds the topmost entry.
    wgpu::Texture nestedMaskTexture;
    wgpu::TextureView nestedMaskView;
    for (auto rit = impl_->clipStack.rbegin(); rit != impl_->clipStack.rend(); ++rit) {
      if (rit->maskResolveView) {
        nestedMaskTexture = rit->maskResolveTexture;
        nestedMaskView = rit->maskResolveView;
        break;
      }
    }

    (void)dev;  // Kept for potential future use; texture allocation
                // routes through `impl_->acquireTexture` now.
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
      wgpu::TextureDescriptor msaaDesc = {};
      wgpu::TextureDescriptor resolveDesc = {};
      wgpu::Texture msaaTexture = makeMsaaTexture("RendererGeodeClipMaskMsaa", msaaDesc);
      wgpu::Texture resolveTexture =
          makeResolveTexture("RendererGeodeClipMaskResolve", resolveDesc);
      if (!resolveTexture || (impl_->device->sampleCount() > 1 && !msaaTexture)) {
        // Release anything we managed to acquire before giving up.
        if (msaaTexture) impl_->releaseTexture(std::move(msaaTexture), msaaDesc);
        if (resolveTexture) impl_->releaseTexture(std::move(resolveTexture), resolveDesc);
        continue;
      }

      // Bind the previously-rendered nested mask (if any) so this
      // layer's fragment shader samples it and intersects.
      if (nestedMaskView) {
        impl_->encoder->setClipMask(nestedMaskTexture, nestedMaskView);
      } else {
        impl_->encoder->clearClipMask();
      }

      impl_->encoder->beginMaskPass(msaaTexture, resolveTexture);
      for (size_t s = it->begin; s < it->end; ++s) {
        const PathShape& shape = clip.clipPaths[s];
        const Transform2d composed =
            clip.clipPathUnitsTransform * shape.parentFromEntity * savedDeviceFromLocalTransform;
        impl_->encoder->setTransform(composed);
        impl_->encoder->fillPathIntoMask(shape.path, shape.fillRule);
      }
      impl_->encoder->endMaskPass();

      nestedMaskTexture = resolveTexture;
      nestedMaskView = resolveTexture.createView();

      // Keep the intermediate textures alive until popClip, paired
      // with their descs for M4.2 pool release.
      if (msaaTexture) {
        entry.maskLayerTextures.push_back({std::move(msaaTexture), msaaDesc});
      }
      entry.maskLayerTextures.push_back({resolveTexture, resolveDesc});

      // The outermost layer (the LAST one processed by this loop,
      // i.e. the FIRST run in `runs`) provides the resolve view the
      // main draws sample as their clip.
      entry.maskResolveTexture = nestedMaskTexture;
      entry.maskResolveView = nestedMaskView;
    }

    // Clear the encoder's internal clip-mask state — the next main
    // pass will rebind via `updateEncoderScissor`.
    impl_->encoder->clearClipMask();
    impl_->encoder->setTransform(savedDeviceFromLocalTransform);
  }

  impl_->clipStack.push_back(std::move(entry));
  impl_->updateEncoderScissor();
}

void RendererGeode::popClip() {
  // M6-B step 3: flush before popping so the batched draws stay
  // inside the clip region they were accumulated under.
  impl_->flushPendingBatch();

  if (!impl_->clipStack.empty()) {
    // Defer release of the mask textures to endFrame — the main
    // encoder that was just drawing under this clip may have recorded
    // samples from `maskResolveTexture` into the frame encoder, and
    // recycling mid-frame could hand the texture to a later acquire
    // before the submit.
    Impl::ClipStackEntry& entry = impl_->clipStack.back();
    for (auto& release : entry.maskLayerTextures) {
      impl_->releaseTextureAtFrameEnd(std::move(release.texture), release.desc);
    }
    entry.maskLayerTextures.clear();
    impl_->clipStack.pop_back();
  }
  impl_->updateEncoderScissor();
}

void RendererGeode::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  impl_->flushPendingBatch();  // M6-B step 3

  // Phase 3d implements all 16 `mix-blend-mode` values: the pushed
  // layer renders normally, and `popIsolatedLayer` switches to a
  // blend-blit compositor that reads a frozen snapshot of the parent
  // target and runs the matching W3C Compositing 1 formula per
  // pixel. `MixBlendMode::Normal` keeps the existing plain
  // source-over composite path.
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      !impl_->encoder) {
    // Headless or degenerate state — drop silently but still push a
    // placeholder frame so popIsolatedLayer stays balanced.
    impl_->layerStack.push_back({});
    return;
  }

  // Allocate offscreen 1-sample resolve + 4× MSAA companion for the
  // layer. All draws issued between push/pop land here; pop composites
  // the resolved 1-sample texture back onto the outer target with the
  // stored opacity.
  wgpu::TextureDescriptor td = {};
  td.label = wgpuLabel("RendererGeodeIsolatedLayer");
  td.size = {static_cast<uint32_t>(impl_->pixelWidth), static_cast<uint32_t>(impl_->pixelHeight),
             1u};
  td.format = impl_->textureFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture layerTexture = impl_->acquireTexture(td);
  if (!layerTexture) {
    impl_->layerStack.push_back({});
    return;
  }

  wgpu::TextureDescriptor msaaDesc = {};
  wgpu::Texture layerMsaaTexture;
  if (impl_->device->sampleCount() > 1) {
    msaaDesc.label = wgpuLabel("RendererGeodeIsolatedLayerMSAA");
    msaaDesc.size = td.size;
    msaaDesc.format = impl_->textureFormat;
    msaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
    msaaDesc.mipLevelCount = 1;
    msaaDesc.sampleCount = 4;
    msaaDesc.dimension = wgpu::TextureDimension::_2D;
    layerMsaaTexture = impl_->acquireTexture(msaaDesc);
    if (!layerMsaaTexture) {
      impl_->releaseTexture(std::move(layerTexture), td);
      impl_->layerStack.push_back({});
      return;
    }
  }

  // Finish the outer encoder so its queued draws land on the saved target
  // before we redirect subsequent work into the offscreen layer. Same
  // shape as `beginPatternTile` — two render-pass submissions ordered
  // serially on the queue.
  impl_->encoder->finish();

  Impl::LayerStackFrame frame;
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.savedMsaaTarget = impl_->msaaTarget;
  frame.layerTexture = layerTexture;
  frame.layerMsaaTexture = layerMsaaTexture;
  frame.layerDesc = td;
  frame.layerMsaaDesc = msaaDesc;
  frame.opacity = opacity;
  frame.blendMode = blendMode;

  impl_->target = layerTexture;
  impl_->msaaTarget = layerMsaaTexture;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      layerMsaaTexture, layerTexture, impl_->frameCommandEncoder);
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);
  impl_->layerStack.push_back(std::move(frame));
  // The layer inherits the outer clip stack — reapply it to the new
  // encoder so scissors carry through.
  impl_->updateEncoderScissor();
}

void RendererGeode::popIsolatedLayer() {
  impl_->flushPendingBatch();  // M6-B step 3
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

  // Restore outer target references.
  impl_->target = frame.savedTarget;
  impl_->msaaTarget = frame.savedMsaaTarget;

  if (frame.blendMode != MixBlendMode::Normal) {
    // Phase 3d: SVG `mix-blend-mode`. The fragment shader needs the
    // parent's current pixels as a backdrop, but WebGPU forbids
    // reading from the render pass's own color attachment. Snapshot
    // the parent's 1-sample resolve target into a separate texture
    // via a CopyTextureToTexture command, then open a fresh parent
    // encoder with `LoadOp::Clear` (NOT Load — the blend shader
    // outputs the final pixel directly, incorporating the snapshot
    // backdrop, so preserving the old contents would double-apply).
    wgpu::TextureDescriptor snapDesc = {};
    snapDesc.label = wgpuLabel("RendererGeodeBlendDstSnapshot");
    snapDesc.size = {static_cast<uint32_t>(impl_->pixelWidth),
                     static_cast<uint32_t>(impl_->pixelHeight), 1u};
    snapDesc.format = impl_->textureFormat;
    snapDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    snapDesc.mipLevelCount = 1;
    snapDesc.sampleCount = 1;
    snapDesc.dimension = wgpu::TextureDimension::_2D;
    wgpu::Texture snapshot = impl_->acquireTexture(snapDesc);

    if (snapshot) {
      // Record the snapshot copy into the shared frame CommandEncoder
      // (design doc 0030 M3). `impl_->encoder->finish()` above already
      // ended the layer's render pass, so it's safe to record a
      // CommandEncoder-level copyTextureToTexture here — no separate
      // CommandEncoder + submit required.
      wgpu::TexelCopyTextureInfo src = {};
      src.texture = frame.savedTarget;
      wgpu::TexelCopyTextureInfo dst = {};
      dst.texture = snapshot;
      const wgpu::Extent3D extent = {static_cast<uint32_t>(impl_->pixelWidth),
                                     static_cast<uint32_t>(impl_->pixelHeight), 1u};
      impl_->frameCommandEncoder.copyTextureToTexture(src, dst, extent);

      // Open a fresh parent encoder that PRESERVES the target's existing
      // contents (the backdrop pre-push — identical to `snapshot` at
      // this point, since we just copied from `savedTarget` above).
      //
      // We must not CLEAR here: if an outer clip/scissor is active,
      // `updateEncoderScissor` will restrict the blend blit to the clip
      // rect, and any pixels outside that rect would remain at the
      // clear color (transparent) — losing the backdrop outside the
      // clip. With Load, out-of-scissor pixels are preserved from the
      // attachment, which already holds the backdrop.
      //
      // No feedback loop: `snapshot` is a copy of `savedTarget`, not an
      // alias, so sampling `snapshot` while writing `savedTarget` is
      // safe.
      auto newEncoder = std::make_unique<geode::GeoEncoder>(
          *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
          frame.savedMsaaTarget, frame.savedTarget, impl_->frameCommandEncoder);
      newEncoder->setLoadPreserve();
      impl_->encoder = std::move(newEncoder);
      impl_->updateEncoderScissor();
      impl_->encoder->blitFullTargetBlended(frame.layerTexture, snapshot,
                                            static_cast<uint32_t>(frame.blendMode), frame.opacity);
      // Defer release: `blitFullTargetBlended` recorded samples from
      // both `frame.layerTexture` and `snapshot` into the shared
      // frameCommandEncoder; they must stay alive until that buffer
      // is submitted at `endFrame`.
      impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
      impl_->releaseTextureAtFrameEnd(std::move(frame.layerMsaaTexture), frame.layerMsaaDesc);
      impl_->releaseTextureAtFrameEnd(std::move(snapshot), snapDesc);
      return;
    }
    // If snapshot allocation failed fall through to the Normal path —
    // at least the layer content shows up even if unblended.
  }

  // Plain premultiplied source-over (the `Normal` case). Create a
  // fresh encoder that preserves its existing contents (LoadOp::Load
  // on the outer MSAA texture, whose state was retained via
  // `StoreOp::Store`). Draw the layer's RESOLVED (1-sample) texture
  // across the entire target with the stored opacity as the
  // compositing alpha.
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.savedMsaaTarget, frame.savedTarget, impl_->frameCommandEncoder);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();
  impl_->encoder->blitFullTarget(frame.layerTexture, frame.opacity);
  // Same deferred-release rationale as the blend-mode branch above.
  impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
  impl_->releaseTextureAtFrameEnd(std::move(frame.layerMsaaTexture), frame.layerMsaaDesc);
}

void RendererGeode::pushFilterLayer(const components::FilterGraph& filterGraph,
                                    const std::optional<Box2d>& filterRegion) {
  impl_->flushPendingBatch();  // M6-B step 3
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      !impl_->encoder || !impl_->filterEngine) {
    // Headless or degenerate state — push a placeholder frame so
    // popFilterLayer stays balanced.
    impl_->filterStack.push_back({});
    return;
  }

  // Compute filter region in device-pixel coordinates. Fall back to the
  // full target if none was specified.
  Box2d region = filterRegion.value_or(
      Box2d(Vector2d::Zero(), Vector2d(impl_->pixelWidth, impl_->pixelHeight)));

  // Compute buffer expansion for spatial-shift primitives (feOffset). When the filter region's
  // device-space AABB extends to negative coordinates (due to skew/rotation), expand the buffer
  // so SourceGraphic captures all content that feOffset can shift into view.
  int filterBufferOffsetX = 0;
  int filterBufferOffsetY = 0;
  const int viewportWidth = impl_->pixelWidth;
  const int viewportHeight = impl_->pixelHeight;

  if (filterRegion.has_value()) {
    const Box2d deviceRegion = impl_->deviceFromLocalTransform.transformBox(*filterRegion);
    const int regionX0 = static_cast<int>(std::floor(deviceRegion.topLeft.x));
    const int regionY0 = static_cast<int>(std::floor(deviceRegion.topLeft.y));

    if ((regionX0 < 0 || regionY0 < 0) && graphHasSpatialShift(filterGraph)) {
      constexpr int kMaxExpansion = 4096;
      if (regionX0 < 0) {
        filterBufferOffsetX = std::min(-regionX0, std::max(0, kMaxExpansion - viewportWidth));
      }
      if (regionY0 < 0) {
        filterBufferOffsetY = std::min(-regionY0, std::max(0, kMaxExpansion - viewportHeight));
      }
    }
  }

  const int bufferWidth = viewportWidth + filterBufferOffsetX;
  const int bufferHeight = viewportHeight + filterBufferOffsetY;

  // Allocate offscreen 1-sample resolve + 4× MSAA companion for the
  // filter layer capture. All draws between push/pop land here; pop runs
  // the filter graph on the resolved texture and composites back.
  wgpu::TextureDescriptor td{};
  td.label = wgpuLabel("RendererGeodeFilterLayer");
  td.size = {static_cast<uint32_t>(bufferWidth), static_cast<uint32_t>(bufferHeight), 1u};
  td.format = impl_->textureFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture layerTexture = impl_->acquireTexture(td);
  if (!layerTexture) {
    impl_->filterStack.push_back({});
    return;
  }

  wgpu::TextureDescriptor msaaDesc{};
  msaaDesc.label = wgpuLabel("RendererGeodeFilterLayerMSAA");
  msaaDesc.size = td.size;
  msaaDesc.format = impl_->textureFormat;
  msaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
  msaaDesc.mipLevelCount = 1;
  msaaDesc.sampleCount = 4;
  msaaDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture layerMsaaTexture = impl_->acquireTexture(msaaDesc);
  if (!layerMsaaTexture) {
    impl_->releaseTexture(std::move(layerTexture), td);
    impl_->filterStack.push_back({});
    return;
  }

  // Finish the outer encoder so its queued draws land on the saved
  // target before we redirect subsequent work into the filter layer.
  impl_->encoder->finish();

  Impl::FilterStackFrame frame;
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.savedMsaaTarget = impl_->msaaTarget;
  frame.layerTexture = layerTexture;
  frame.layerMsaaTexture = layerMsaaTexture;
  frame.layerDesc = td;
  frame.layerMsaaDesc = msaaDesc;
  frame.filterGraph = filterGraph;
  frame.filterRegion = region;
  frame.deviceFromFilter = impl_->deviceFromLocalTransform;
  frame.filterBufferOffsetX = filterBufferOffsetX;
  frame.filterBufferOffsetY = filterBufferOffsetY;
  frame.savedClipStack = impl_->clipStack;

  impl_->target = layerTexture;
  impl_->msaaTarget = layerMsaaTexture;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      layerMsaaTexture, layerTexture, impl_->frameCommandEncoder);
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);

  // Apply the buffer offset to the current transform so content at negative device coordinates
  // renders into the expanded region. Subsequent setTransform calls will also pick up the offset.
  if (filterBufferOffsetX != 0 || filterBufferOffsetY != 0) {
    impl_->deviceFromLocalTransform =
        impl_->deviceFromLocalTransform * Transform2d::Translate(filterBufferOffsetX, filterBufferOffsetY);
  }

  // Per SVG Filter Effects § filter region: pixels outside the filter region are
  // transparent black for filter-primitive purposes. Scissor source draws to the
  // filter region's device-space AABB so the SourceGraphic fed into filter
  // primitives (e.g. feGaussianBlur) respects that boundary.
  Impl::ClipStackEntry filterClipEntry;
  filterClipEntry.pixelRect = impl_->deviceFromLocalTransform.transformBox(region);
  filterClipEntry.valid = true;
  impl_->clipStack.push_back(std::move(filterClipEntry));

  impl_->filterStack.push_back(std::move(frame));
  // Refresh scissor to the intersection of the outer clip stack and the filter region.
  impl_->updateEncoderScissor();
}

void RendererGeode::popFilterLayer() {
  impl_->flushPendingBatch();  // M6-B step 3
  if (impl_->filterStack.empty()) {
    return;
  }
  Impl::FilterStackFrame frame = std::move(impl_->filterStack.back());
  impl_->filterStack.pop_back();
  impl_->clipStack = frame.savedClipStack;

  if (!frame.layerTexture) {
    return;  // Placeholder frame from the headless/error path.
  }

  // Finish the filter layer's render pass so the texture is ready.
  if (impl_->encoder) {
    impl_->encoder->finish();
  }

  // The filter engine runs on its own CommandEncoder + submit, so the
  // layer-texture writes we just recorded into `frameCommandEncoder`
  // must be submitted to the GPU BEFORE the filter engine tries to
  // sample from `layerTexture`. Flush the shared encoder (1 extra
  // submit, paid only on frames that use a filter).
  impl_->flushFrameCommandEncoder();

  // When the filter buffer was expanded to capture negative-coordinate content, adjust
  // deviceFromFilter to include the offset so the filter engine interprets coordinates correctly.
  const Transform2d bufferDeviceFromFilter =
      (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0)
          ? frame.deviceFromFilter *
                Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY)
          : frame.deviceFromFilter;

  // Run the filter graph on the captured layer texture.
  wgpu::Texture filteredTexture = frame.layerTexture;
  if (impl_->filterEngine && !frame.filterGraph.empty()) {
    filteredTexture = impl_->filterEngine->execute(frame.filterGraph, frame.layerTexture,
                                                   frame.filterRegion, bufferDeviceFromFilter);
  }

  // Restore outer target and create a fresh encoder that preserves its
  // existing contents. Composite the filtered texture back with full
  // opacity (filter results are already premultiplied).
  impl_->target = frame.savedTarget;
  impl_->msaaTarget = frame.savedMsaaTarget;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.savedMsaaTarget, frame.savedTarget, impl_->frameCommandEncoder);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();
  if (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0) {
    // The filter result is in an expanded texture. Extract the viewport-sized region at the
    // buffer offset using a GPU texture copy, then blit the viewport-sized result.
    const uint32_t vpW = static_cast<uint32_t>(impl_->pixelWidth);
    const uint32_t vpH = static_cast<uint32_t>(impl_->pixelHeight);

    wgpu::TextureDescriptor vpDesc{};
    vpDesc.label = wgpuLabel("RendererGeodeFilterViewport");
    vpDesc.size = {vpW, vpH, 1u};
    vpDesc.format = kFilterIntermediateFormat;
    vpDesc.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding;
    vpDesc.mipLevelCount = 1;
    vpDesc.sampleCount = 1;
    vpDesc.dimension = wgpu::TextureDimension::_2D;
    wgpu::Texture viewportTexture = impl_->device->device().createTexture(vpDesc);

    if (viewportTexture) {
      wgpu::CommandEncoder copyEncoder = impl_->device->device().createCommandEncoder();
      wgpu::TexelCopyTextureInfo src = {};
      src.texture = filteredTexture;
      src.origin = {static_cast<uint32_t>(frame.filterBufferOffsetX),
                    static_cast<uint32_t>(frame.filterBufferOffsetY), 0u};
      wgpu::TexelCopyTextureInfo dst = {};
      dst.texture = viewportTexture;
      const wgpu::Extent3D extent = {vpW, vpH, 1u};
      copyEncoder.copyTextureToTexture(src, dst, extent);
      wgpu::CommandBuffer copyCmd = copyEncoder.finish();
      impl_->device->queue().submit(1, &copyCmd);

      impl_->encoder->blitFullTarget(viewportTexture, 1.0);
    }
  } else {
    // TODO(geode): Clip the composite to the filter region per SVG 2 §15.5.
    // `frame.filterRegion` is passed in from the driver in user-space
    // coordinates, not pixel-space; we'd need to transform by the current
    // CTM snapshot before using it as a scissor. Skipping for this PR —
    // all current feGaussianBlur resvg tests pass without the clip.
    impl_->encoder->blitFullTarget(filteredTexture, 1.0);
  }
  // Defer release to endFrame: `blitFullTarget` recorded a sample from
  // `filteredTexture` (which is `frame.layerTexture` when the filter
  // graph is empty) into the frame encoder. Filter-engine-owned
  // intermediates are tracked separately by `GeodeFilterEngine` and
  // covered by M5; we only recycle the layer capture pair here.
  impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
  impl_->releaseTextureAtFrameEnd(std::move(frame.layerMsaaTexture), frame.layerMsaaDesc);
}

void RendererGeode::pushMask(const std::optional<Box2d>& maskBounds) {
  impl_->flushPendingBatch();  // M6-B step 3
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      !impl_->encoder || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    // Headless / degenerate — push a placeholder so popMask stays balanced.
    impl_->maskStack.push_back({});
    return;
  }

  const auto allocTexturePair = [&](const char* label, const char* msaaLabel,
                                    wgpu::Texture& outResolve, wgpu::TextureDescriptor& outDesc,
                                    wgpu::Texture& outMsaa, wgpu::TextureDescriptor& outMsaaDesc) {
    outDesc = wgpu::TextureDescriptor{};
    outDesc.label = wgpuLabel(label);
    outDesc.size = {static_cast<uint32_t>(impl_->pixelWidth),
                    static_cast<uint32_t>(impl_->pixelHeight), 1u};
    outDesc.format = impl_->textureFormat;
    outDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                    wgpu::TextureUsage::CopySrc;
    outDesc.mipLevelCount = 1;
    outDesc.sampleCount = 1;
    outDesc.dimension = wgpu::TextureDimension::_2D;
    outResolve = impl_->acquireTexture(outDesc);

    if (impl_->device->sampleCount() > 1) {
      outMsaaDesc = wgpu::TextureDescriptor{};
      outMsaaDesc.label = wgpuLabel(msaaLabel);
      outMsaaDesc.size = outDesc.size;
      outMsaaDesc.format = impl_->textureFormat;
      outMsaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
      outMsaaDesc.mipLevelCount = 1;
      outMsaaDesc.sampleCount = 4;
      outMsaaDesc.dimension = wgpu::TextureDimension::_2D;
      outMsaa = impl_->acquireTexture(outMsaaDesc);
    }
  };

  Impl::MaskStackFrame frame;
  allocTexturePair("RendererGeodeMaskCapture", "RendererGeodeMaskCaptureMSAA", frame.maskTexture,
                   frame.maskDesc, frame.maskMsaaTexture, frame.maskMsaaDesc);
  allocTexturePair("RendererGeodeMaskContent", "RendererGeodeMaskContentMSAA", frame.contentTexture,
                   frame.contentDesc, frame.contentMsaaTexture, frame.contentMsaaDesc);
  const bool needsMsaa = impl_->device->sampleCount() > 1;
  if (!frame.maskTexture || (needsMsaa && !frame.maskMsaaTexture) || !frame.contentTexture ||
      (needsMsaa && !frame.contentMsaaTexture)) {
    impl_->maskStack.push_back({});
    return;
  }
  frame.maskBounds = maskBounds;
  frame.maskBoundsTransform = impl_->deviceFromLocalTransform;

  // Flush the outer encoder's pending draws so they land before we
  // redirect subsequent commands into the mask capture.
  impl_->encoder->finish();

  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.savedMsaaTarget = impl_->msaaTarget;
  frame.phase = Impl::MaskStackFrame::Phase::Capturing;

  impl_->target = frame.maskTexture;
  impl_->msaaTarget = frame.maskMsaaTexture;
  auto captureEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.maskMsaaTexture, frame.maskTexture, impl_->frameCommandEncoder);
  captureEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(captureEncoder);
  impl_->maskStack.push_back(std::move(frame));
  impl_->updateEncoderScissor();
}

void RendererGeode::transitionMaskToContent() {
  impl_->flushPendingBatch();  // M6-B step 3
  if (impl_->maskStack.empty()) {
    return;
  }
  Impl::MaskStackFrame& frame = impl_->maskStack.back();
  if (frame.phase != Impl::MaskStackFrame::Phase::Capturing) {
    return;
  }
  const bool needsMsaa = impl_->device->sampleCount() > 1;
  if (!frame.contentTexture || (needsMsaa && !frame.contentMsaaTexture) || !impl_->encoder) {
    frame.phase = Impl::MaskStackFrame::Phase::Content;
    return;
  }

  // Flush the mask-capture encoder so the mask texture is ready to
  // sample in popMask.
  impl_->encoder->finish();

  impl_->target = frame.contentTexture;
  impl_->msaaTarget = frame.contentMsaaTexture;
  auto contentEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.contentMsaaTexture, frame.contentTexture, impl_->frameCommandEncoder);
  contentEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(contentEncoder);
  frame.phase = Impl::MaskStackFrame::Phase::Content;
  impl_->updateEncoderScissor();
}

void RendererGeode::popMask() {
  impl_->flushPendingBatch();  // M6-B step 3
  if (impl_->maskStack.empty()) {
    return;
  }
  Impl::MaskStackFrame frame = std::move(impl_->maskStack.back());
  impl_->maskStack.pop_back();

  if (!frame.savedEncoder) {
    // Placeholder frame from the headless path — nothing to do.
    return;
  }

  // Finish the content encoder so its resolve is ready to sample.
  if (impl_->encoder) {
    impl_->encoder->finish();
  }

  // Restore the outer target and reopen a new encoder with load-
  // preserve on the saved MSAA state.
  impl_->target = frame.savedTarget;
  impl_->msaaTarget = frame.savedMsaaTarget;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.savedMsaaTarget, frame.savedTarget, impl_->frameCommandEncoder);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();

  // Lift the raw mask-bounds rect into device-pixel space using the
  // transform captured at pushMask time. This handles `maskUnits ==
  // userSpaceOnUse` (where bounds are in user space and need the
  // element's world transform applied) and `objectBoundingBox`
  // (where the driver has already folded the bbox mapping into the
  // bounds, but the outer world transform still applies).
  std::optional<Box2d> pixelMaskBounds;
  if (frame.maskBounds.has_value()) {
    pixelMaskBounds = frame.maskBoundsTransform.transformBox(*frame.maskBounds);
  }

  // Composite `content * luminance(mask)` onto the outer target.
  impl_->encoder->blitFullTargetMasked(frame.contentTexture, frame.maskTexture, pixelMaskBounds);

  // Defer release to endFrame — `blitFullTargetMasked` recorded
  // samples from both `contentTexture` and `maskTexture` into the
  // frame encoder. MSAA companions had no post-render samples but
  // are bucketed alongside their resolve for symmetry.
  impl_->releaseTextureAtFrameEnd(std::move(frame.maskTexture), frame.maskDesc);
  impl_->releaseTextureAtFrameEnd(std::move(frame.maskMsaaTexture), frame.maskMsaaDesc);
  impl_->releaseTextureAtFrameEnd(std::move(frame.contentTexture), frame.contentDesc);
  impl_->releaseTextureAtFrameEnd(std::move(frame.contentMsaaTexture), frame.contentMsaaDesc);
}

void RendererGeode::beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) {
  impl_->flushPendingBatch();  // M6-B step 3
  if (!impl_->device || !impl_->pipeline) {
    return;
  }

  // Raster resolution: use the composition of the current transform and the
  // tile transform to estimate how many device pixels one tile unit maps to,
  // then scale the tile texture accordingly. Using device pixels directly as
  // a 1:1 fallback would under-sample patterns that are scaled up before
  // tiling. We clamp to a minimum of 1 pixel per axis so zero-size tiles
  // never allocate a zero-extent texture.
  //
  // A 2× supersample factor matches RendererTinySkia's
  // kPatternSupersampleScale. Without it, small tiles (e.g. a 2×2 tile
  // scaled 10× → 20×20 device px) produce a 20×20-texel texture whose
  // bilinear-sampled circle edges visibly drift from the reference golden
  // that was rendered at 40×40 texels. The extra resolution lets the
  // MSAA-resolved tile capture finer edge transitions, closing the gap.
  constexpr double kPatternSupersampleScale = 2.0;
  const Transform2d deviceFromPattern = impl_->deviceFromLocalTransform * targetFromPattern;
  const double scaleX = std::hypot(deviceFromPattern.data[0], deviceFromPattern.data[1]);
  const double scaleY = std::hypot(deviceFromPattern.data[2], deviceFromPattern.data[3]);
  auto boundedPx = [](double v) {
    if (!(v > 0.0) || !std::isfinite(v)) {
      return 1;
    }
    constexpr double kMaxTileDim = 4096.0;
    return std::max(1, static_cast<int>(std::ceil(std::min(v, kMaxTileDim))));
  };
  const double ssX = (scaleX > 0.0 ? scaleX : 1.0) * kPatternSupersampleScale;
  const double ssY = (scaleY > 0.0 ? scaleY : 1.0) * kPatternSupersampleScale;
  const int tilePixelWidth = boundedPx(tileRect.width() * ssX);
  const int tilePixelHeight = boundedPx(tileRect.height() * ssY);

  // 1-sample resolve target for the pattern tile (this is what the Slug
  // fill shader samples when the tile is later used as paint).
  wgpu::TextureDescriptor td = {};
  td.label = wgpuLabel("RendererGeodePatternTile");
  td.size = {static_cast<uint32_t>(tilePixelWidth), static_cast<uint32_t>(tilePixelHeight), 1u};
  td.format = impl_->textureFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture tileTexture = impl_->device->device().createTexture(td);
  impl_->device->countTexture();
  if (!tileTexture) {
    return;
  }

  // 4× MSAA companion render target — shares the same encoder lifetime
  // as the tile; resolved into `tileTexture` at pass end. Skipped on
  // the alpha-coverage path (sampleCount == 1).
  wgpu::Texture tileMsaaTexture;
  if (impl_->device->sampleCount() > 1) {
    wgpu::TextureDescriptor tileMsaaDesc = {};
    tileMsaaDesc.label = wgpuLabel("RendererGeodePatternTileMSAA");
    tileMsaaDesc.size = td.size;
    tileMsaaDesc.format = impl_->textureFormat;
    tileMsaaDesc.usage = wgpu::TextureUsage::RenderAttachment;
    tileMsaaDesc.mipLevelCount = 1;
    tileMsaaDesc.sampleCount = 4;
    tileMsaaDesc.dimension = wgpu::TextureDimension::_2D;
    tileMsaaTexture = impl_->device->device().createTexture(tileMsaaDesc);
    impl_->device->countTexture();
    if (!tileMsaaTexture) {
      return;
    }
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
  frame.savedMsaaTarget = impl_->msaaTarget;
  frame.savedDeviceFromLocalTransform = impl_->deviceFromLocalTransform;
  frame.savedDeviceFromLocalTransformStack = std::move(impl_->deviceFromLocalTransformStack);
  frame.savedPixelWidth = impl_->pixelWidth;
  frame.savedPixelHeight = impl_->pixelHeight;
  frame.tileRect = tileRect;
  frame.targetFromPattern = targetFromPattern;
  frame.tileTexture = tileTexture;
  frame.tileMsaaTexture = tileMsaaTexture;
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
  impl_->msaaTarget = tileMsaaTexture;
  impl_->deviceFromLocalTransformStack.clear();
  // Initialise the current transform to the raster scale so direct draws
  // issued before the driver's next `setTransform` still land in the
  // correct place on the tile texture.
  const Vector2d& rasterScale = impl_->patternStack.back().rasterScale;
  impl_->deviceFromLocalTransform = Transform2d::Scale(rasterScale.x, rasterScale.y);

  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      tileMsaaTexture, tileTexture, impl_->frameCommandEncoder);
  // Transparent clear so unpainted tile pixels contribute nothing.
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);

  // Compose the pattern raster scale into targetFromPattern so it takes
  // pattern-pixel coordinates to target units. We record the raster scale
  // in the frame itself and apply it in endPatternTile.
}

void RendererGeode::endPatternTile(bool forStroke) {
  impl_->flushPendingBatch();  // M6-B step 3
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
  impl_->msaaTarget = frame.savedMsaaTarget;
  impl_->pixelWidth = frame.savedPixelWidth;
  impl_->pixelHeight = frame.savedPixelHeight;
  impl_->deviceFromLocalTransform = frame.savedDeviceFromLocalTransform;
  impl_->deviceFromLocalTransformStack = std::move(frame.savedDeviceFromLocalTransformStack);

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
  // On the 1-sample alpha-coverage path (Intel Arc Vulkan) `frame.savedMsaaTarget`
  // is intentionally null; `GeoEncoder` handles the no-MSAA case. Only require
  // the MSAA attachment when the device actually uses multisampling.
  const bool needsMsaa = impl_->device && impl_->device->sampleCount() > 1;
  if (impl_->device && impl_->pipeline && impl_->gradientPipeline && impl_->imagePipeline &&
      frame.savedTarget && (!needsMsaa || frame.savedMsaaTarget)) {
    auto newEncoder = std::make_unique<geode::GeoEncoder>(
        *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
        frame.savedMsaaTarget, frame.savedTarget, impl_->frameCommandEncoder);
    // Preserve existing target contents: the pattern subtree may have
    // submitted work on the outer target *before* the pattern tile opened
    // (via the finish() in beginPatternTile), so we must not clear it
    // again. GeoEncoder's constructor defaults to LoadOp::Clear with a
    // transparent clearValue; call setLoadPreserve() to switch it to
    // LoadOp::Load for the next render pass.
    newEncoder->setLoadPreserve();
    impl_->encoder = std::move(newEncoder);
    // Re-apply the active clip stack to the freshly-created encoder.
    // The scissor state lived on the OLD encoder (finished inside
    // beginPatternTile) and doesn't carry over automatically — without
    // this call, a pattern capture triggered from inside a viewport
    // or `<use>` clip would leak the subsequent pattern fill outside
    // that clip rect. Mirrors the scissor restore in push/
    // popIsolatedLayer.
    impl_->updateEncoderScissor();
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
  // `frame.savedDeviceFromLocalTransform` is the path→device transform that was live at
  // `beginPatternTile` time — i.e., the outer element's deviceFromLocalTransform
  // including the viewBox→canvas scale. We stash it on the slot so
  // `buildPatternPaint` can cancel it out of the live `deviceFromLocalTransform`
  // at the upcoming fill draw, leaving the pattern sample in the pattern's
  // user-space frame (where `tileSize` is expressed).
  slot.deviceFromPathAtCapture = frame.savedDeviceFromLocalTransform;
  if (forStroke) {
    impl_->patternStrokePaint = std::move(slot);
  } else {
    impl_->patternFillPaint = std::move(slot);
  }
}

void RendererGeode::setPaint(const PaintParams& paint) {
  impl_->paint = paint;
}

void RendererGeode::drawPath(const PathShape& path, const StrokeParams& stroke) {
  // M6-B detection (design doc 0030 §M6 Bullet 2): when a `<use>`
  // draws a path that was also just drawn by the previous call —
  // same source entity, same paint — this is exactly the case an
  // instancing pass would collapse into one GPU draw. Count it here
  // so the benefit of a future batcher is measurable before the
  // batcher ships. Null source (non-driver callers) never matches,
  // so editor overlay / convenience `drawRect` calls don't skew the
  // counter.
  if (path.sourceEntity.entity() != entt::null &&
      path.sourceEntity.entity() == impl_->lastDrawSourceEntity) {
    ++impl_->counters.sameSourceDrawPairs;
  }
  impl_->lastDrawSourceEntity = path.sourceEntity.entity();

  // M2 cache lookup for the fill encode. Null `sourceEntity` (editor
  // overlay, test-harness direct draws) returns nullptr and `GeoEncoder`
  // falls back to the inline encode path.
  const geode::EncodedPath* fillEncoded =
      impl_->getFillEncode(path.sourceEntity, path.path, path.fillRule);

  // M6-B step 3: try to append to a pending `<use>`-batch. Preconditions:
  //  - Source entity valid (non-null handle).
  //  - Fill encode cache hit (shared across all instances).
  //  - Solid paint (gradient / pattern need per-draw uniforms today).
  //  - No active pattern-fill handoff from the driver.
  //  - No stroke (we can't defer a fill while the stroke runs on top).
  //
  // When all hold: append this draw's `deviceFromLocalTransform` into the pending
  // batch (flushing + restarting if the paint/source key differs) and
  // return early. A later state change (pushClip, popLayer, endFrame,
  // setPaint-different-key, non-batchable draw) flushes as one instanced
  // draw.
  const bool hasStroke = !(stroke.strokeWidth <= 0.0 ||
                           std::holds_alternative<PaintServer::None>(impl_->paint.stroke));
  const bool batchable = !hasStroke && fillEncoded != nullptr &&
                         path.sourceEntity.entity() != entt::null &&
                         std::holds_alternative<PaintServer::Solid>(impl_->paint.fill) &&
                         !impl_->patternFillPaint.has_value() && impl_->encoder != nullptr;
  if (batchable) {
    const auto& solid = std::get<PaintServer::Solid>(impl_->paint.fill);
    const css::RGBA color = solid.color.resolve(impl_->paint.currentColor.rgba(),
                                                static_cast<float>(impl_->paint.fillOpacity));
    impl_->tryAppendOrStartBatch(path.sourceEntity.entity(), path.path, color, path.fillRule,
                                 fillEncoded);
    return;
  }

  // Non-batchable: flush whatever's pending, then emit normally.
  impl_->flushPendingBatch();
  impl_->fillResolved(path.path, path.fillRule, fillEncoded);

  // Mirror fillResolved's no-op safety: if there's no encoder (headless
  // device init failed, zero-pixel viewport, or draw-before-beginFrame),
  // the stroke branch must bail too. Otherwise the encoder dereference
  // below crashes.
  if (!impl_->encoder) {
    return;
  }

  if (stroke.strokeWidth <= 0.0 || std::holds_alternative<PaintServer::None>(impl_->paint.stroke)) {
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
  // Fill rule: for closed subpaths, strokeToFill emits the outer and
  // inner contours as two *same-winding* closed subpaths (not opposite),
  // so NonZero would over-fill the interior and EvenOdd is required to
  // get a hollow ring. For open subpaths, `strokeToFill` emits one
  // closed polygon — but with overlapping start/end caps (e.g. the
  // resvg `stroke-linecap/open-path-with-*` tests where the 4-point path
  // `M 150 50 l 0 80 -100 -40 100 -40` ends at its start), the inside-
  // miter shortcut in `emitJoin` creates a self-intersecting polygon
  // whose interior has the wrong winding under EvenOdd (the first-
  // segment rectangle drops out). NonZero handles that case correctly
  // because the overlapping winding still sums to non-zero.
  //
  // The M2 cache (`GeodePathCacheComponent::strokeSlot`) memoizes the
  // `strokeToFill` output + its encode + the derived fill rule, keyed
  // by `StrokeStyle` equality. A cache hit skips all three computations.
  const StrokeStyle strokeStyle = toStrokeStyle(stroke);
  const Impl::StrokeDerived strokeDerived =
      impl_->getStrokeDerived(path.sourceEntity, path.path, strokeStyle);
  if (!strokeDerived.strokedPath) {
    return;
  }
  const Path& strokedOutline = *strokeDerived.strokedPath;

  // Pattern dispatch comes first: a stroke pattern slot was populated by the
  // driver via `endPatternTile(forStroke=true)` and consumed here.
  if (impl_->patternStrokePaint.has_value()) {
    impl_->syncTransform();
    const double opacity = impl_->paint.strokeOpacity;
    impl_->encoder->fillPathPattern(strokedOutline, strokeDerived.fillRule,
                                    impl_->buildPatternPaint(*impl_->patternStrokePaint, opacity),
                                    strokeDerived.encoded);
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
                                strokeDerived.fillRule, strokeDerived.encoded);
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
  impl_->flushPendingBatch();  // M6-B step 3
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

void RendererGeode::drawText(Registry& registry, const components::ComputedTextComponent& text,
                             const TextParams& params) {
  impl_->flushPendingBatch();  // M6-B step 3
#ifdef DONNER_TEXT_ENABLED
  if (!impl_->device || !impl_->encoder || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return;
  }
  if (!registry.ctx().contains<TextEngine>()) {
    if (impl_->verbose && !impl_->warnedText) {
      std::cerr << "RendererGeode: TextEngine not available in registry context\n";
      impl_->warnedText = true;
    }
    return;
  }

  auto& textEngine = registry.ctx().get<TextEngine>();

  // Use cached layout runs from `ComputedTextGeometryComponent` when
  // available; otherwise lay out fresh via the engine. This matches
  // the pattern in `RendererTinySkia::drawText`.
  std::vector<TextRun> runs;
  if (params.textRootEntity != entt::null) {
    if (const auto* cache =
            registry.try_get<components::ComputedTextGeometryComponent>(params.textRootEntity)) {
      runs = cache->runs;
    }
  }
  if (runs.empty()) {
    const TextLayoutParams layoutParams = toTextLayoutParams(params);
    runs = textEngine.layout(text, layoutParams);
  }

  const float textFontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // Resolve a default fill colour from the text-element-level paint
  // state. Per-span fills override below when present.
  std::optional<css::RGBA> defaultFill = impl_->resolveSolidFill();
  if (!defaultFill.has_value()) {
    // No solid fill and no fallback -- text is effectively invisible.
    // Still walk through in case a per-span fill kicks in.
    defaultFill = css::RGBA(0, 0, 0, 0);
  }

  const css::RGBA currentColor = impl_->paint.currentColor.rgba();

  // The element-level fill is already scaled by the top-level
  // `setPaint` call, but per-span fill overrides need their own
  // `span.fillOpacity * span.opacity` applied.
  const auto resolveSpanFill = [&](size_t runIndex) -> css::RGBA {
    if (runIndex >= text.spans.size()) {
      return *defaultFill;
    }
    const auto& span = text.spans[runIndex];
    const float opacityScale = static_cast<float>(span.fillOpacity * span.opacity);
    if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedFill)) {
      return solid->color.resolve(currentColor, opacityScale);
    }
    if (const auto* ref = std::get_if<components::PaintResolvedReference>(&span.resolvedFill)) {
      if (ref->fallback.has_value()) {
        return ref->fallback->resolve(currentColor, opacityScale);
      }
    }
    return *defaultFill;
  };

  // Snapshot the encoder's current transform so we can restore it if
  // per-glyph rotations mess with it. `fillPath` honours
  // `impl_->deviceFromLocalTransform` via `setTransform`, and the glyph
  // outline coordinates are already mapped into the text element's
  // local space by the transformPath call below -- so we want the
  // encoder to use the element's deviceFromLocalTransform unchanged.
  impl_->encoder->setTransform(impl_->deviceFromLocalTransform);

  for (size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
    const auto& run = runs[runIndex];
    if (run.font == FontHandle()) {
      continue;
    }

    float spanFontSizePx = textFontSizePx;
    if (runIndex < text.spans.size() && text.spans[runIndex].fontSize.value != 0.0) {
      spanFontSizePx = static_cast<float>(text.spans[runIndex].fontSize.toPixels(
          params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
    }

    const float scale = textEngine.scaleForPixelHeight(run.font, spanFontSizePx);
    if (scale <= 0.0f) {
      continue;
    }
    if (textEngine.isBitmapOnly(run.font)) {
      // Bitmap-only (color emoji) fonts need the `GeodeTextureEncoder`
      // path, which drawText doesn't wire up yet. Skip the run so the
      // rest of the text still renders.
      continue;
    }

    const css::RGBA spanFill = resolveSpanFill(runIndex);
    if (spanFill.a == 0) {
      continue;
    }

    for (const auto& glyph : run.glyphs) {
      if (glyph.glyphIndex == 0) {
        continue;  // `.notdef` -- skip to match tiny-skia.
      }

      Path glyphPath =
          textEngine.glyphOutline(run.font, glyph.glyphIndex, scale * glyph.fontSizeScale);
      if (glyphPath.empty()) {
        continue;
      }

      // Build the local-space transform that takes the raw glyph
      // outline (baseline-origin, em-scaled) to its placed position.
      // Order matches `RendererTinySkia::drawText`:
      //   stretchScale -> rotate (around glyph origin) -> translate.
      Transform2d glyphFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
      if (glyph.rotateDegrees != 0.0) {
        const double radians = glyph.rotateDegrees * MathConstants<double>::kPi / 180.0;
        glyphFromLocal = Transform2d::Rotate(radians) * glyphFromLocal;
      }
      if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
        glyphFromLocal =
            Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY) * glyphFromLocal;
      }

      const Path placed = transformPath(glyphPath, glyphFromLocal);
      impl_->encoder->fillPath(placed, spanFill, FillRule::NonZero);
    }
  }
#else
  (void)registry;
  (void)text;
  (void)params;
  if (impl_->verbose && !impl_->warnedText) {
    std::cerr << "RendererGeode: text rendering requires DONNER_TEXT_ENABLED\n";
    impl_->warnedText = true;
  }
#endif
}

std::unique_ptr<RendererInterface> RendererGeode::createOffscreenInstance() const {
  if (!impl_->device) {
    return nullptr;
  }
  return std::make_unique<RendererGeode>(impl_->device, impl_->verbose);
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
  bd.label = wgpuLabel("RendererGeodeReadback");
  bd.size = static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height);
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = impl_->device->device().createBuffer(bd);
  impl_->device->countBuffer();

  // Copy texture → readback buffer.
  wgpu::CommandEncoder enc = impl_->device->device().createCommandEncoder();
  wgpu::TexelCopyTextureInfo src = {};
  src.texture = impl_->target;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};
  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = readback;
  dst.layout.bytesPerRow = bytesPerRow;
  dst.layout.rowsPerImage = height;
  wgpu::Extent3D copySize = {width, height, 1};
  enc.copyTextureToBuffer(src, dst, copySize);

  wgpu::CommandBuffer cmd = enc.finish();
  impl_->device->queue().submit(1, &cmd);
  impl_->device->countSubmit();

  // Map for read. wgpu-native's C++ wrapper (`webgpu.hpp`) only exposes the
  // `BufferMapCallbackInfo` form of `mapAsync`, which takes a raw C function
  // pointer + two void*'s rather than a std::function. We stash the "done"
  // flag in `userdata1` so the callback can flip it and we can spin until it
  // flips. wgpu-native guarantees `wgpuDevicePoll(wait=true)` drains pending
  // callbacks before returning — a single `poll(true, nullptr)` is enough to
  // wait for the map to complete.
  struct MapState {
    bool done = false;
    bool ok = false;
  } mapState;
  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                      void* /*userdata2*/) {
    auto* s = static_cast<MapState*>(userdata1);
    s->ok = (status == WGPUMapAsyncStatus_Success);
    s->done = true;
  };
  mapCb.userdata1 = &mapState;
  mapCb.userdata2 = nullptr;
  // Browser WebGPU fires `mapAsync` completion via the JS Promise
  // microtask — there is no wgpu-native-style "poll" on the instance.
  // `AllowProcessEvents` would require an explicit `wgpuInstanceProcessEvents`
  // call, which we never make (our Emscripten `wgpuDevicePoll` stub only
  // yields via `emscripten_sleep`). Use `AllowSpontaneous` so the browser
  // can fire the callback as soon as the Promise resolves, during the
  // microtask tick that runs while we're sleeping. wgpu-native also
  // accepts spontaneous mode and fires callbacks during `wgpuDevicePoll`.
  mapCb.mode = wgpu::CallbackMode::AllowSpontaneous;
  readback.mapAsync(wgpu::MapMode::Read, 0, bd.size, mapCb);
  int pollIter = 0;
  while (!mapState.done) {
    impl_->device->device().poll(true, nullptr);
    ++pollIter;
    if (pollIter > 2000) {  // 2000 * 5ms = 10s bail-out.
      break;
    }
  }
  if (!mapState.ok) {
    return bitmap;
  }

  const uint8_t* mapped = static_cast<const uint8_t*>(readback.getConstMappedRange(0, bd.size));

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
  readback.unmap();
  return bitmap;
}

}  // namespace donner::svg
