#include "tiny_skia/Canvas.h"

#include "tiny_skia/Painter.h"

namespace tiny_skia {

void Canvas::fillRect(const Rect& rect, const Paint& paint, Transform transform, const Mask* mask) {
  Painter::fillRect(view_, rect, paint, transform, mask);
}

void Canvas::fillPath(const Path& path, const Paint& paint, FillRule fillRule, Transform transform,
                      const Mask* mask) {
  Painter::fillPath(view_, path, paint, fillRule, transform, mask);
}

void Canvas::strokePath(const Path& path, const Paint& paint, const Stroke& stroke,
                        Transform transform, const Mask* mask) {
  Painter::strokePath(view_, path, paint, stroke, transform, mask);
}

void Canvas::drawPixmap(std::int32_t x, std::int32_t y, PixmapView src, const PixmapPaint& ppaint,
                        Transform transform, const Mask* mask) {
  Painter::drawPixmap(view_, x, y, src, ppaint, transform, mask);
}

void Canvas::applyMask(const Mask& mask) { Painter::applyMask(view_, mask); }

}  // namespace tiny_skia
