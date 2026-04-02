#pragma once

/// @file Canvas.h
/// @brief Drawing surface that operates on a Pixmap or MutablePixmapView.

#include <cstdint>

#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/Pattern.h"

namespace tiny_skia {

/// Drawing surface backed by a mutable pixel buffer.
///
/// Canvas wraps a MutablePixmapView (or a Pixmap) and provides the
/// primary drawing API: fillRect, fillPath, strokePath, drawPixmap,
/// and applyMask.
///
/// ```cpp
/// auto pixmap = Pixmap::fromSize(500, 500).value();
/// Canvas canvas(pixmap);
/// canvas.fillPath(path, paint, FillRule::Winding);
/// ```
class Canvas {
 public:
  /// Constructs a Canvas from a mutable view.
  explicit Canvas(MutablePixmapView view) : view_(view) {}

  /// Constructs a Canvas from an owned Pixmap.
  explicit Canvas(Pixmap& pixmap) : view_(pixmap.mutableView()) {}

  /// Fills an axis-aligned rectangle.
  void fillRect(const Rect& rect, const Paint& paint, Transform transform = Transform::identity(),
                const Mask* mask = nullptr);

  /// Fills a path using the given fill rule.
  void fillPath(const Path& path, const Paint& paint, FillRule fillRule,
                Transform transform = Transform::identity(), const Mask* mask = nullptr);

  /// Strokes a path with the given stroke settings.
  void strokePath(const Path& path, const Paint& paint, const Stroke& stroke,
                  Transform transform = Transform::identity(), const Mask* mask = nullptr);

  /// Composites a source pixmap onto this canvas at offset (x, y).
  void drawPixmap(std::int32_t x, std::int32_t y, PixmapView src, const PixmapPaint& paint,
                  Transform transform = Transform::identity(), const Mask* mask = nullptr);

  /// Applies a mask to already-drawn content.
  void applyMask(const Mask& mask);

 private:
  MutablePixmapView view_;
};

}  // namespace tiny_skia
