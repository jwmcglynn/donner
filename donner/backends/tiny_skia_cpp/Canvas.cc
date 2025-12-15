#include "donner/backends/tiny_skia_cpp/Canvas.h"

#include <utility>

namespace donner::backends::tiny_skia_cpp {

Canvas::Canvas(Pixmap pixmap) : pixmap_(std::move(pixmap)) {}

Expected<Canvas, std::string> Canvas::Create(int width, int height) {
  Pixmap pixmap = Pixmap::Create(width, height);
  if (!pixmap.isValid()) {
    return Expected<Canvas, std::string>::Failure("Invalid canvas dimensions");
  }

  return Expected<Canvas, std::string>::Success(Canvas(std::move(pixmap)));
}

Expected<std::monostate, std::string> Canvas::drawPath(const svg::PathSpline& spline,
                                                       const Paint& paint, FillRule fillRule,
                                                       const Transform& transform,
                                                       const Mask* clipMask) {
  return FillPath(spline, paint, pixmap_, fillRule, transform, clipMask);
}

Expected<std::monostate, std::string> Canvas::strokePath(const svg::PathSpline& spline,
                                                         const Stroke& stroke, const Paint& paint,
                                                         const Transform& transform,
                                                         const Mask* clipMask) {
  return StrokePath(spline, stroke, paint, pixmap_, transform, clipMask);
}

Expected<std::monostate, std::string> Canvas::drawPixmap(int x, int y, const Pixmap& source,
                                                         const PixmapPaint& paint,
                                                         const Transform& transform,
                                                         const Mask* clipMask) {
  return DrawPixmap(x, y, source, paint, pixmap_, transform, clipMask);
}

void Canvas::clear(Color color) {
  if (!pixmap_.isValid()) {
    return;
  }

  const size_t strideBytes = pixmap_.strideBytes();
  uint8_t* const data = pixmap_.data();

  for (int y = 0; y < pixmap_.height(); ++y) {
    uint8_t* row = data + strideBytes * static_cast<size_t>(y);
    for (int x = 0; x < pixmap_.width(); ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      row[offset] = color.r;
      row[offset + 1] = color.g;
      row[offset + 2] = color.b;
      row[offset + 3] = color.a;
    }
  }
}

}  // namespace donner::backends::tiny_skia_cpp
