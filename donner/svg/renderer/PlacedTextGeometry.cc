#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "donner/base/MathUtils.h"
#include "donner/base/Utils.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/DecorationWorkBudget.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/text/TextPathWarp.h"

namespace donner::svg {

TextLayoutParams ToTextLayoutParams(const TextParams& params) {
  TextLayoutParams layoutParams;
  layoutParams.fontFamilies = params.fontFamilies;
  layoutParams.fontSize = params.fontSize;
  layoutParams.viewBox = params.viewBox;
  layoutParams.fontMetrics = params.fontMetrics;
  layoutParams.textAnchor = params.textAnchor;
  layoutParams.writingMode = params.writingMode;
  layoutParams.letterSpacingPx = params.letterSpacingPx;
  layoutParams.wordSpacingPx = params.wordSpacingPx;
  layoutParams.textLength = params.textLength;
  layoutParams.lengthAdjust = params.lengthAdjust;
  layoutParams.inlineSizePx = params.inlineSizePx;
  layoutParams.textIndentPx = params.textIndentPx;
  return layoutParams;
}

TextDrawGeometry PrepareTextDrawGeometry(Registry& registry,
                                         const components::ComputedTextComponent& text,
                                         const TextParams& params, TextEngine& textEngine) {
  TextDrawGeometry geometry;
  if (params.preparedTextDraw) {
    geometry.runs = params.preparedTextDraw->runs;
  } else if (params.textRootEntity != entt::null) {
    if (const auto* cached =
            registry.try_get<components::ComputedTextGeometryComponent>(params.textRootEntity)) {
      geometry.runs = cached->runs;
    }
  }
  if (geometry.runs.empty() && !params.preparedTextDraw) {
    geometry.runs = textEngine.layout(text, ToTextLayoutParams(params));
  }
  geometry.elementBounds = params.preparedTextDraw ? params.preparedTextDraw->elementBounds
                                                   : ComputeTextBounds(textEngine, geometry.runs);
  ClearUnpaintedSpanGlyphs(text, params.spanEffectOwner, geometry.runs);
  return geometry;
}

Path TransformPath(const Path& path, const Transform2d& destinationFromSource) {
  PathBuilder builder;
  const auto points = path.points();

  for (const Path::Command& command : path.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(destinationFromSource.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(destinationFromSource.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(destinationFromSource.transformPosition(points[command.pointIndex]),
                       destinationFromSource.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(destinationFromSource.transformPosition(points[command.pointIndex]),
                        destinationFromSource.transformPosition(points[command.pointIndex + 1]),
                        destinationFromSource.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }

  return builder.build();
}

Transform2d GlyphPlacementTransform(const TextGlyph& glyph) {
  return Transform2d::Translate(glyph.xPosition, glyph.yPosition);
}

GlyphOutlineAndPlacement UnplacedGlyphOutline(const TextEngine& textEngine, FontHandle font,
                                              const TextGlyph& glyph, float scale) {
  GlyphOutlineAndPlacement result;

  // Position. Computed even for an empty outline so the placement stays a pure
  // function of the glyph's position fields.
  result.glyphFromLocal = GlyphPlacementTransform(glyph);

  if (glyph.glyphIndex == 0) {
    return result;  // `.notdef` -- caller skips.
  }

  // 1. Raw outline at the glyph's effective scale.
  result.outline = textEngine.glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale,
                                           glyph.allowSyntheticOblique);
  if (result.outline.empty()) {
    return result;  // No vector outline (e.g. bitmap-only) -- caller handles.
  }

  // 2. Stretch the raw outline (lengthAdjust=spacingAndGlyphs), matching
  //    tiny-skia: the stretch is baked into the outline *before* placement, so
  //    when a per-glyph rotation is present the stretch axes follow the glyph,
  //    not the post-rotation frame.
  if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
    result.outline =
        TransformPath(result.outline, Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
  }

  // 3. Rotation, baked into the OUTLINE rather than left in the placement.
  //
  //    A renderer that keeps this outline and carries `glyphFromLocal` as a
  //    draw transform is choosing the coordinate space its rasteriser works in.
  //    Leaving a rotation in that transform tilts the outline's own space
  //    against the pixel grid, and a rasteriser that evaluates coverage with
  //    per-axis pixel footprints then measures a rotated glyph through a
  //    coarser footprint than an upright one - visibly softer edges on
  //    `rotate` and on `textPath`, which rotates every glyph. Baking it here
  //    keeps that space axis-aligned, and the rotation stays part of the
  //    glyph's identity so one angle costs one cached outline.
  if (glyph.rotateDegrees != 0.0) {
    result.outline = TransformPath(
        result.outline,
        Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0));
  }

  return result;
}

Path WarpGlyphOutline(const Path& outline, const StretchedTextPath& mapping, const TextGlyph& glyph,
                      double tolerance) {
  return WarpTextPathOutline(outline, mapping, glyph, tolerance);
}

namespace {

/// Largest singular value of the linear part of \p deviceFromLocal, ignoring translation. The
/// default renderer must not depend on Geode packages, so this does not share Geode's tolerance
/// helper.
double LinearMaxScale(const Transform2d& deviceFromLocal) {
  const double a = deviceFromLocal.data[0];
  const double b = deviceFromLocal.data[1];
  const double c = deviceFromLocal.data[2];
  const double d = deviceFromLocal.data[3];
  const double col0 = a * a + b * b;
  const double col1 = c * c + d * d;
  const double cross = a * c + b * d;
  const double diff = col0 - col1;
  const double sigmaSquared = 0.5 * (col0 + col1 + std::sqrt(diff * diff + 4.0 * cross * cross));
  return sigmaSquared > 0.0 ? std::sqrt(sigmaSquared) : 0.0;
}

}  // namespace

double TextWarpToleranceFor(const Transform2d& deviceFromLocal) {
  constexpr double kDeviceTolerance = 0.1;
  constexpr double kMinTolerance = 1e-4;
  // The largest singular value is at least the largest coefficient magnitude, so a coefficient
  // this large already reaches the minimum. Rejecting it here also keeps `LinearMaxScale` from
  // squaring a huge coefficient to infinity, or a non-finite one to NaN, which it reports as zero
  // scale.
  for (int i = 0; i < 4; ++i) {
    if (!(std::abs(deviceFromLocal.data[i]) < kDeviceTolerance / kMinTolerance)) {
      return kMinTolerance;
    }
  }
  // The largest singular value of the linear part bounds how far any local error can move in
  // device space; translation does not magnify it.
  const double scale = LinearMaxScale(deviceFromLocal);
  return std::clamp(kDeviceTolerance / std::max(scale, 1.0), kMinTolerance,
                    Path::kLocalFlattenTolerance);
}

Path PlacedGlyphOutline(const TextEngine& textEngine, FontHandle font, const TextRun& run,
                        const TextGlyph& glyph, float scale, double warpTolerance) {
  if (glyph.glyphIndex == 0) {
    return Path();  // `.notdef` -- caller skips.
  }

  // Deliberately NOT built from `UnplacedGlyphOutline`: this composes the
  // placement into ONE affine and transforms the outline once, and the CPU
  // backend's golden images are pinned to that exact arithmetic. Splitting it
  // into a rotate pass and a translate pass is the same mapping but not the
  // same rounding.
  Path glyphPath = textEngine.glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale,
                                           glyph.allowSyntheticOblique);
  if (glyphPath.empty()) {
    return glyphPath;  // No vector outline (e.g. bitmap-only) -- caller handles.
  }

  if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
    glyphPath =
        TransformPath(glyphPath, Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
  }

  if (run.stretchedTextPath && glyph.textPathOrigin) {
    const double localRotation = glyph.textPathLocalRotateDegrees.value_or(0.0);
    if (localRotation != 0.0) {
      glyphPath = TransformPath(
          glyphPath, Transform2d::Rotate(localRotation * MathConstants<double>::kPi / 180.0));
    }
    return WarpGlyphOutline(glyphPath, *run.stretchedTextPath, glyph, warpTolerance);
  }

  // Translate to the baseline origin and rotate about it, as one transform.
  Transform2d glyphFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
  if (glyph.rotateDegrees != 0.0) {
    glyphFromLocal = Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
                     glyphFromLocal;
  }

  return TransformPath(glyphPath, glyphFromLocal);
}

GlyphFaceScale ResolveGlyphFace(const TextEngine& textEngine, const TextRun& run,
                                const TextGlyph& glyph, float runScale, bool runBitmapOnly) {
  if (!glyph.font) {
    return {run.font, runScale, runBitmapOnly};
  }
  return {glyph.font, textEngine.scaleForPixelHeight(glyph.font, run.usedFontSizePx),
          textEngine.isBitmapOnly(glyph.font)};
}

void ClearUnpaintedSpanGlyphs(const components::ComputedTextComponent& text,
                              entt::entity spanEffectOwner, std::vector<TextRun>& runs) {
  const size_t count = std::min(runs.size(), text.spans.size());
  for (size_t runIndex = 0; runIndex < count; ++runIndex) {
    const auto& span = text.spans[runIndex];
    if (span.visibility != Visibility::Visible || span.effectOwner != spanEffectOwner) {
      runs[runIndex].glyphs.clear();
    }
  }
}

std::optional<Box2d> TextGlyphCellBounds(const TextRun& run, const TextGlyph& glyph, double emTop,
                                         double emBottom) {
  if (!run.stretchedTextPath || !glyph.textPathOrigin) {
    const double geometricScaleY = glyph.stretchScaleY;
    return Box2d::FromXYWH(glyph.xPosition, glyph.yPosition - emTop * geometricScaleY,
                           glyph.xAdvance, (emTop + emBottom) * geometricScaleY);
  }
  const Path warpedCell = PlaceStretchedTextPathCell(run, glyph, emTop, emBottom);
  return warpedCell.empty() ? std::nullopt : std::optional(warpedCell.bounds());
}

Box2d ComputeTextBounds(const TextEngine& textEngine, const std::vector<TextRun>& runs) {
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();

  for (size_t runIdx = 0; runIdx < runs.size(); ++runIdx) {
    const auto& run = runs[runIdx];

    const float runFontSizePx = run.usedFontSizePx;

    // Em-box vertical extent from font v-metrics (ascent above baseline,
    // |descent| below), not the raw font size.
    const float runScale =
        run.font ? textEngine.scaleForPixelHeight(run.font, runFontSizePx) : 0.0f;
    double emTop = static_cast<double>(runFontSizePx);  // fallback: full size above baseline
    double emBottom = 0.0;                              // fallback: baseline
    if (run.font && runScale > 0.0f) {
      const FontVMetrics metrics = textEngine.fontVMetrics(run.font);
      emTop = static_cast<double>(metrics.ascent) * runScale;
      emBottom = -static_cast<double>(metrics.descent) * runScale;
    }

    for (const auto& glyph : run.glyphs) {
      if (glyph.glyphIndex == 0) {
        continue;
      }
      const auto bounds = TextGlyphCellBounds(run, glyph, emTop, emBottom);
      if (!bounds) {
        continue;
      }
      minX = std::min(minX, bounds->topLeft.x);
      maxX = std::max(maxX, bounds->bottomRight.x);
      minY = std::min(minY, bounds->topLeft.y);
      maxY = std::max(maxY, bounds->bottomRight.y);
    }
  }

  if (minX < maxX && minY < maxY) {
    return Box2d({minX, minY}, {maxX, maxY});
  }
  return Box2d();
}

namespace {
struct DecorationMetrics {
  double ascent = 0;
  double descent = 0;
  double underlinePosition = 0;
  double underlineWidth = 0;
  double strikePosition = 0;
  double strikeWidth = 0;
  double automaticWidth = 0;
};

DecorationMetrics GetDecorationMetrics(const TextRun& run,
                                       const components::TextDecorationOrigin& origin,
                                       float fontSize, const TextEngine& engine) {
  const auto font = origin.font.value_or(ResolvedTextFont{run.font, fontSize});
  const double scale = engine.scaleForPixelHeight(font.font, font.usedSizePx);
  const double emScale = engine.scaleForEmToPixels(font.font, font.usedSizePx);
  const auto metrics = engine.fontVMetrics(font.font);
  DecorationMetrics result;
  result.ascent = static_cast<double>(metrics.ascent) * scale;
  result.descent = static_cast<double>(metrics.descent) * scale;
  if (const auto underline = engine.underlineMetrics(font.font)) {
    result.underlinePosition = -underline->position * emScale;
    result.underlineWidth = underline->thickness * emScale;
  }
  if (const auto strike = engine.strikeoutMetrics(font.font)) {
    result.strikePosition = -strike->position * emScale;
    result.strikeWidth = strike->thickness * emScale;
  }
  result.automaticWidth =
      result.underlineWidth > 0 ? result.underlineWidth : (result.ascent - result.descent) / 18.0;
  return result;
}

bool DecorationGlyphVisible(const TextGlyph& glyph) {
  return glyph.glyphIndex != 0 && glyph.xAdvance > 0;
}

bool ValidDecorationCoordinate(double value) {
  return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max() / 4.0;
}

bool AppendDecorationRect(PathBuilder& builder, double left, double top, double right,
                          double bottom) {
  const std::array<double, 4> values = {left, top, right, bottom};
  if (!(right > left && bottom > top) ||
      !std::all_of(values.begin(), values.end(), ValidDecorationCoordinate)) {
    return false;
  }
  builder.moveTo({left, top})
      .lineTo({right, top})
      .lineTo({right, bottom})
      .lineTo({left, bottom})
      .closePath();
  return true;
}

/// Sanity bound on dash, dot or half-wave elements in one segment, which keeps the integer element
/// counts in range. The per-draw work allowance, charged point by point, is the effective limit.
constexpr double kMaximumStyledDecorationElements =
    static_cast<double>(Path::kMaximumGeometryQueryWork);

/// Number of dashes, dots or half-waves a styled segment draws; 1 for solid and double lines.
double StyledElementCount(TextDecorationStyle style, double length, double width) {
  switch (style) {
    case TextDecorationStyle::Solid:
    case TextDecorationStyle::Double: return 1;
    // Dashes three thicknesses long, starting every five.
    case TextDecorationStyle::Dashed: return std::ceil(length / (5 * width));
    // Round dots of diameter `width`, one every two thicknesses; at least one dot.
    case TextDecorationStyle::Dotted: return std::max(1.0, std::floor((length / width + 1) / 2));
    // Half-waves near 2 * width long that exactly divide the segment.
    case TextDecorationStyle::Wavy: return std::max(1.0, std::round(length / (2 * width)));
  }
  UTILS_UNREACHABLE();
}

/// Points \ref AppendStyledDecoration adds for one segment, or 0 when it draws nothing. Callers
/// size their bounded builders and charge the shared work allowance with this count.
std::size_t StyledDecorationPoints(TextDecorationStyle style, double length, double width) {
  if (!(length > 0 && width > 0) || !std::isfinite(length) || !std::isfinite(width)) {
    return 0;
  }
  const double count = StyledElementCount(style, length, width);
  if (!(count <= kMaximumStyledDecorationElements)) {
    return 0;
  }
  constexpr std::size_t kRectPoints = 4;     // moveTo and three lineTo.
  constexpr std::size_t kCirclePoints = 13;  // moveTo and four cubic segments.
  const auto elements = static_cast<std::size_t>(count);
  switch (style) {
    case TextDecorationStyle::Solid: return kRectPoints;
    case TextDecorationStyle::Double: return 2 * kRectPoints;
    case TextDecorationStyle::Dashed: return elements * kRectPoints;
    case TextDecorationStyle::Dotted: return elements * kCirclePoints;
    // Two edges of quadratic half-waves, the start point and the joining line.
    case TextDecorationStyle::Wavy: return 2 + 4 * elements;
  }
  UTILS_UNREACHABLE();
}

/// Charge `points` against the shared work allowance; exhaust it and refuse when it is too small.
bool ChargeDecorationWork(std::size_t points, std::size_t& remainingWork) {
  if (points > remainingWork) {
    remainingWork = 0;
    return false;
  }
  remainingWork -= points;
  return true;
}

/// Two bands of thickness `width` with a gap of `width`: the second lies away from the text, and a
/// line-through straddles its solid position.
bool AppendDoubleDecoration(PathBuilder& builder, TextDecoration line, double left, double top,
                            double right, double width) {
  double first = top;
  double second = top + 2 * width;
  if (line == TextDecoration::Overline) {
    second = top - 2 * width;
  } else if (line == TextDecoration::LineThrough) {
    first = top - width;
    second = top + width;
  }
  const bool appendedFirst = AppendDecorationRect(builder, left, first, right, first + width);
  const bool appendedSecond = AppendDecorationRect(builder, left, second, right, second + width);
  return appendedFirst || appendedSecond;
}

/// `count` dashes three thicknesses long, starting every five; the last dash ends at `right`. The
/// position is computed from the index rather than accumulated, so a period below the coordinate
/// precision cannot stall the loop or exceed the counted points.
bool AppendDashedDecoration(PathBuilder& builder, double left, double top, double right,
                            double width, int count) {
  bool appended = false;
  for (int i = 0; i < count; ++i) {
    const double x = left + 5 * width * i;
    appended |= AppendDecorationRect(builder, x, top, std::min(x + 3 * width, right), top + width);
  }
  return appended;
}

/// `count` round dots of diameter `width`, one every two thicknesses.
bool AppendDottedDecoration(PathBuilder& builder, double left, double top, double width,
                            int count) {
  const double radius = width / 2;
  for (int i = 0; i < count; ++i) {
    builder.addCircle({left + radius + 2 * width * i, top + radius}, radius);
  }
  return true;
}

/// A band of thickness `width` between two copies of the same wave, built from `halfWaves`
/// quadratic half-waves of amplitude `width` that exactly divide [left, right], so the wave starts
/// and ends on its center line.
bool AppendWavyDecoration(PathBuilder& builder, double left, double top, double right, double width,
                          int halfWaves) {
  const double half = (right - left) / halfWaves;
  const double lower = top + width;
  const auto control = [&](int index, double base) {
    // Even half-waves bulge toward -y and odd ones toward +y. A quadratic curve reaches half of
    // its control offset, so the control sits at twice the amplitude.
    const double direction = index % 2 == 0 ? -1.0 : 1.0;
    return Vector2d(left + half * (index + 0.5), base + direction * 2 * width);
  };
  builder.moveTo({left, top});
  for (int i = 0; i < halfWaves; ++i) {
    builder.quadTo(control(i, top), {left + half * (i + 1), top});
  }
  builder.lineTo({right, lower});
  for (int i = halfWaves - 1; i >= 0; --i) {
    builder.quadTo(control(i, lower), {left + half * i, lower});
  }
  builder.closePath();
  return true;
}

/**
 * Append one decoration line in segment coordinates. The solid band spans x in [left, right] and y
 * in [top, top + width]; the other styles derive their geometry from that band as documented on
 * \ref TextDecorationStyle.
 */
bool AppendStyledDecoration(PathBuilder& builder, TextDecorationStyle style, TextDecoration line,
                            double left, double top, double right, double width) {
  const std::array<double, 4> extents = {left, right, top - 2 * width, top + 3 * width};
  if (StyledDecorationPoints(style, right - left, width) == 0 ||
      !std::all_of(extents.begin(), extents.end(), ValidDecorationCoordinate)) {
    return false;
  }
  const int count = static_cast<int>(StyledElementCount(style, right - left, width));
  switch (style) {
    case TextDecorationStyle::Solid:
      return AppendDecorationRect(builder, left, top, right, top + width);
    case TextDecorationStyle::Double:
      return AppendDoubleDecoration(builder, line, left, top, right, width);
    case TextDecorationStyle::Dashed:
      return AppendDashedDecoration(builder, left, top, right, width, count);
    case TextDecorationStyle::Dotted:
      return AppendDottedDecoration(builder, left, top, width, count);
    case TextDecorationStyle::Wavy:
      return AppendWavyDecoration(builder, left, top, right, width, count);
  }
  UTILS_UNREACHABLE();
}

double DecorationEnd(const TextRun& run, std::size_t index) {
  const auto& glyph = run.glyphs[index];
  const auto next =
      std::find_if(run.glyphs.begin() + index + 1, run.glyphs.end(), DecorationGlyphVisible);
  return next == run.glyphs.end() ? glyph.xPosition + glyph.xAdvance
                                  : std::min(glyph.xPosition + glyph.xAdvance, next->xPosition);
}

double DecorationOffset(TextDecoration line, const DecorationMetrics& metrics) {
  if (line == TextDecoration::Underline) {
    return metrics.underlinePosition != 0 ? metrics.underlinePosition : -metrics.descent * 0.4;
  }
  if (line == TextDecoration::Overline) {
    return -metrics.ascent;
  }
  return metrics.strikePosition != 0 ? metrics.strikePosition : -metrics.ascent * 0.35;
}

double MultipleDecorationAdjustment(TextDecoration line, TextDecoration lines,
                                    std::size_t originCount, double width) {
  const int count = static_cast<int>(hasFlag(lines, TextDecoration::Underline)) +
                    static_cast<int>(hasFlag(lines, TextDecoration::Overline)) +
                    static_cast<int>(hasFlag(lines, TextDecoration::LineThrough));
  if (originCount != 1 || count <= 1) {
    return 0;
  }
  return line == TextDecoration::Overline      ? width * 1.5
         : line == TextDecoration::LineThrough ? -width
                                               : 0;
}
struct DecorationBand {
  TextDecoration line;
  TextDecorationStyle style;
  TextDecoration lines;
  TextDecorationThickness thickness;
  std::size_t originCount;
  double automatic;
  double underline;
  double offset;
};

std::pair<double, double> ResolveDecorationBand(const DecorationBand& band,
                                                const Transform2d& deviceFromSegment) {
  const double baselineLength = std::hypot(deviceFromSegment.data[0], deviceFromSegment.data[1]);
  double scale = 0;
  if (baselineLength > 0 && std::isfinite(baselineLength)) {
    // Only the normal's perpendicular component separates the transformed parallel edges.
    scale = std::abs((deviceFromSegment.data[0] / baselineLength) * deviceFromSegment.data[3] -
                     (deviceFromSegment.data[1] / baselineLength) * deviceFromSegment.data[2]);
  }
  const double width =
      ResolveDecorationThickness(band.thickness, band.automatic, band.underline, scale);
  return {band.offset - width / 2 +
              MultipleDecorationAdjustment(band.line, band.lines, band.originCount, width),
          width};
}

void EmitRotatedDecoration(const TextRun& run, const DecorationBand& band,
                           const Transform2d& deviceFromLocal, std::size_t& remainingWork,
                           const std::function<void(const Path&)>& emit) {
  for (std::size_t index = 0; index < run.glyphs.size(); ++index) {
    const auto& glyph = run.glyphs[index];
    if (!DecorationGlyphVisible(glyph)) {
      continue;
    }
    Transform2d localFromSegment = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
    if (glyph.rotateDegrees != 0) {
      localFromSegment =
          Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
          localFromSegment;
    }
    const auto [top, width] = ResolveDecorationBand(band, localFromSegment * deviceFromLocal);
    const double length = DecorationEnd(run, index) - glyph.xPosition;
    const std::size_t points = StyledDecorationPoints(band.style, length, width);
    if (points == 0) {
      continue;
    }
    if (!ChargeDecorationWork(points, remainingWork)) {
      return;
    }
    PathBuilder builder(points);
    if (!AppendStyledDecoration(builder, band.style, band.line, 0, top, length, width)) {
      continue;
    }
    const Path path = builder.build().transformed(localFromSegment);
    if (std::all_of(path.points().begin(), path.points().end(), [](const Vector2d& point) {
          return ValidDecorationCoordinate(point.x) && ValidDecorationCoordinate(point.y);
        })) {
      emit(path);
    }
  }
}

/// Call `segment(left, baseline, right)` for each straight decoration segment of a run: one span
/// from the first to the last rendered glyph when they share a baseline, else one per glyph.
template <typename SegmentFn>
void ForEachStraightSegment(const TextRun& run, const SegmentFn& segment) {
  const auto first = std::find_if(run.glyphs.begin(), run.glyphs.end(), DecorationGlyphVisible);
  const auto last = std::find_if(run.glyphs.rbegin(), run.glyphs.rend(), DecorationGlyphVisible);
  if (first == run.glyphs.end() || last == run.glyphs.rend()) {
    return;
  }
  const bool sameBaseline =
      std::all_of(run.glyphs.begin(), run.glyphs.end(), [&](const auto& glyph) {
        return !DecorationGlyphVisible(glyph) ||
               std::abs(glyph.yPosition - first->yPosition) < 1e-6;
      });
  if (sameBaseline) {
    segment(first->xPosition, first->yPosition, last->xPosition + last->xAdvance);
    return;
  }
  for (std::size_t index = 0; index < run.glyphs.size(); ++index) {
    const auto& glyph = run.glyphs[index];
    if (DecorationGlyphVisible(glyph)) {
      segment(glyph.xPosition, glyph.yPosition, DecorationEnd(run, index));
    }
  }
}

void EmitStraightDecoration(const TextRun& run, const DecorationBand& band, double top,
                            double width, std::size_t& remainingWork,
                            const std::function<void(const Path&)>& emit) {
  // Size the bounded builder for every segment before appending any of them.
  detail::DecorationWorkBudget<std::size_t> budget(remainingWork);
  ForEachStraightSegment(run, [&](double left, double, double right) {
    budget.add(StyledDecorationPoints(band.style, right - left, width));
  });
  remainingWork = budget.remaining();
  if (budget.exceeded() || budget.points() == 0) {
    return;
  }
  PathBuilder builder(budget.points());
  ForEachStraightSegment(run, [&](double left, double baseline, double right) {
    (void)AppendStyledDecoration(builder, band.style, band.line, left, baseline + top, right,
                                 width);
  });
  if (!builder.empty()) {
    emit(builder.build());
  }
}

}  // namespace

void EmitTextDecorationPaths(const TextRun& run, const components::TextDecorationOrigin& origin,
                             TextDecoration emittedLines, std::size_t originCount, float fontSize,
                             const TextEngine& engine, const Transform2d& deviceFromLocal,
                             std::size_t& remainingWork,
                             const std::function<void(const Path&)>& emit) {
  const TextDecoration lines = origin.lines & emittedLines;
  const std::size_t lineCount =
      static_cast<std::size_t>(hasFlag(lines, TextDecoration::Underline)) +
      static_cast<std::size_t>(hasFlag(lines, TextDecoration::Overline)) +
      static_cast<std::size_t>(hasFlag(lines, TextDecoration::LineThrough));
  if (lineCount == 0) {
    return;
  }
  // Charge 4 work units per glyph and emitted line, so the beneath and above passes together
  // charge an origin once for each of its lines.
  if (run.glyphs.size() > remainingWork / (4 * lineCount)) {
    remainingWork = 0;
    return;
  }
  remainingWork -= run.glyphs.size() * 4 * lineCount;
  const DecorationMetrics metrics = GetDecorationMetrics(run, origin, fontSize, engine);
  const bool rotated = std::any_of(run.glyphs.begin(), run.glyphs.end(),
                                   [](const auto& glyph) { return glyph.rotateDegrees != 0; });
  for (const auto line :
       {TextDecoration::Underline, TextDecoration::Overline, TextDecoration::LineThrough}) {
    if (!hasFlag(origin.lines, line) || !hasFlag(emittedLines, line)) {
      continue;
    }
    const double automatic = line == TextDecoration::LineThrough && metrics.strikeWidth > 0
                                 ? metrics.strikeWidth
                                 : metrics.automaticWidth;
    const DecorationBand band{
        line,        origin.style, origin.lines,           origin.thickness,
        originCount, automatic,    metrics.underlineWidth, DecorationOffset(line, metrics)};
    if (rotated) {
      EmitRotatedDecoration(run, band, deviceFromLocal, remainingWork, emit);
    } else {
      const auto [top, width] = ResolveDecorationBand(band, deviceFromLocal);
      EmitStraightDecoration(run, band, top, width, remainingWork, emit);
    }
  }
}

}  // namespace donner::svg
