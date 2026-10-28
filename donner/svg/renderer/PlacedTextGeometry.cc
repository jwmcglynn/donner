#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "donner/base/MathUtils.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
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
  result.outline = textEngine.glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale);
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
  Path glyphPath = textEngine.glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale);
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
                           const Transform2d& deviceFromLocal,
                           const std::function<void(const Path&)>& emit) {
  for (std::size_t index = 0; index < run.glyphs.size(); ++index) {
    const auto& glyph = run.glyphs[index];
    if (!DecorationGlyphVisible(glyph)) {
      continue;
    }
    Transform2d transform = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
    if (glyph.rotateDegrees != 0) {
      transform =
          Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) * transform;
    }
    const auto [top, width] = ResolveDecorationBand(band, transform * deviceFromLocal);
    PathBuilder builder(4);
    if (!AppendDecorationRect(builder, 0, top, DecorationEnd(run, index) - glyph.xPosition,
                              top + width)) {
      continue;
    }
    const Path path = builder.build().transformed(transform);
    if (std::all_of(path.points().begin(), path.points().end(), [](const Vector2d& point) {
          return ValidDecorationCoordinate(point.x) && ValidDecorationCoordinate(point.y);
        })) {
      emit(path);
    }
  }
}

void EmitStraightDecoration(const TextRun& run, double top, double width,
                            const std::function<void(const Path&)>& emit) {
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
  PathBuilder builder(run.glyphs.size() * 4);
  if (sameBaseline) {
    (void)AppendDecorationRect(builder, first->xPosition, first->yPosition + top,
                               last->xPosition + last->xAdvance, first->yPosition + top + width);
  } else {
    for (std::size_t index = 0; index < run.glyphs.size(); ++index) {
      const auto& glyph = run.glyphs[index];
      if (DecorationGlyphVisible(glyph)) {
        (void)AppendDecorationRect(builder, glyph.xPosition, glyph.yPosition + top,
                                   DecorationEnd(run, index), glyph.yPosition + top + width);
      }
    }
  }
  if (!builder.empty()) {
    emit(builder.build());
  }
}

}  // namespace

void EmitTextDecorationPaths(const TextRun& run, const components::TextDecorationOrigin& origin,
                             std::size_t originCount, float fontSize, const TextEngine& engine,
                             const Transform2d& deviceFromLocal, std::size_t& remainingWork,
                             const std::function<void(const Path&)>& emit) {
  if (run.glyphs.size() > remainingWork / 12) {
    remainingWork = 0;
    return;
  }
  remainingWork -= run.glyphs.size() * 12;
  const DecorationMetrics metrics = GetDecorationMetrics(run, origin, fontSize, engine);
  const bool rotated = std::any_of(run.glyphs.begin(), run.glyphs.end(),
                                   [](const auto& glyph) { return glyph.rotateDegrees != 0; });
  for (const auto line :
       {TextDecoration::Underline, TextDecoration::Overline, TextDecoration::LineThrough}) {
    if (!hasFlag(origin.lines, line)) {
      continue;
    }
    const double automatic = line == TextDecoration::LineThrough && metrics.strikeWidth > 0
                                 ? metrics.strikeWidth
                                 : metrics.automaticWidth;
    const DecorationBand band{line,
                              origin.lines,
                              origin.thickness,
                              originCount,
                              automatic,
                              metrics.underlineWidth,
                              DecorationOffset(line, metrics)};
    if (rotated) {
      EmitRotatedDecoration(run, band, deviceFromLocal, emit);
    } else {
      const auto [top, width] = ResolveDecorationBand(band, deviceFromLocal);
      EmitStraightDecoration(run, top, width, emit);
    }
  }
}

}  // namespace donner::svg
