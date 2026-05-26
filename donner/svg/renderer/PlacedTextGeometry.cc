#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <algorithm>
#include <limits>

#include "donner/base/MathUtils.h"

namespace donner::svg {

Path TransformPath(const Path& path, const Transform2d& transform) {
  PathBuilder builder;
  const auto points = path.points();

  for (const Path::Command& command : path.commands()) {
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

Path PlacedGlyphOutline(const TextEngine& textEngine, FontHandle font, const TextGlyph& glyph,
                        float scale) {
  if (glyph.glyphIndex == 0) {
    return Path();  // `.notdef` -- caller skips.
  }

  // 1. Raw outline at the glyph's effective scale.
  Path glyphPath = textEngine.glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale);
  if (glyphPath.empty()) {
    return glyphPath;  // No vector outline (e.g. bitmap-only) -- caller handles.
  }

  // 2. Stretch the raw outline (lengthAdjust=spacingAndGlyphs), matching
  //    tiny-skia: the stretch is baked into the outline *before* placement, so
  //    when a per-glyph rotation is present the stretch axes follow the glyph,
  //    not the post-rotation frame.
  if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
    glyphPath =
        TransformPath(glyphPath, Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
  }

  // 3. Position: translate to the baseline origin, then rotate about it. As a
  //    composed transform this is `Rotate * Translate` (translate applied
  //    first), matching `RendererTinySkia::drawText`.
  Transform2d glyphFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
  if (glyph.rotateDegrees != 0.0) {
    glyphFromLocal = Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
                     glyphFromLocal;
  }

  return TransformPath(glyphPath, glyphFromLocal);
}

Box2d ComputeTextBounds(const TextEngine& textEngine, const std::vector<TextRun>& runs,
                        std::span<const components::ComputedTextComponent::TextSpan> spans,
                        const Box2d& viewBox, const FontMetrics& fontMetrics, float fontSizePx) {
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();

  for (size_t runIdx = 0; runIdx < runs.size(); ++runIdx) {
    const auto& run = runs[runIdx];

    // Per-run font size (spans may override the text element's font size).
    float runFontSizePx = fontSizePx;
    if (runIdx < spans.size() && spans[runIdx].fontSize.value != 0.0) {
      runFontSizePx = static_cast<float>(
          spans[runIdx].fontSize.toPixels(viewBox, fontMetrics, Lengthd::Extent::Mixed));
    }

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
      minX = std::min(minX, glyph.xPosition);
      maxX = std::max(maxX, glyph.xPosition + glyph.xAdvance);
      minY = std::min(minY, glyph.yPosition - emTop);
      maxY = std::max(maxY, glyph.yPosition + emBottom);
    }
  }

  if (minX < maxX && minY < maxY) {
    return Box2d({minX, minY}, {maxX, maxY});
  }
  return Box2d();
}

}  // namespace donner::svg
