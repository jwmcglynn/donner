#include "donner/svg/renderer/PlacedTextGeometry.h"

#include "donner/base/MathUtils.h"

namespace donner::svg {

Path transformPath(const Path& path, const Transform2d& transform) {
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

Path placedGlyphOutline(const TextEngine& textEngine, FontHandle font, const TextGlyph& glyph,
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
        transformPath(glyphPath, Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
  }

  // 3. Position: translate to the baseline origin, then rotate about it. As a
  //    composed transform this is `Rotate * Translate` (translate applied
  //    first), matching `RendererTinySkia::drawText`.
  Transform2d glyphFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
  if (glyph.rotateDegrees != 0.0) {
    glyphFromLocal = Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
                     glyphFromLocal;
  }

  return transformPath(glyphPath, glyphFromLocal);
}

}  // namespace donner::svg
