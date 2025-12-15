#pragma once
/// @file

#include <string>

#include "donner/backends/tiny_skia_cpp/Color.h"
#include "donner/backends/tiny_skia_cpp/Expected.h"
#include "donner/backends/tiny_skia_cpp/Painter.h"
#include "donner/backends/tiny_skia_cpp/Pixmap.h"
#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "donner/backends/tiny_skia_cpp/Transform.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::backends::tiny_skia_cpp {

/** Simple canvas wrapper around an owned Pixmap. */
class Canvas {
public:
  Canvas() = default;

  /// Allocates a canvas with the given dimensions.
  static Expected<Canvas, std::string> Create(int width, int height);

  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  Canvas(Canvas&&) noexcept = default;
  Canvas& operator=(Canvas&&) noexcept = default;

  /// Image width in pixels.
  int width() const { return pixmap_.width(); }

  /// Image height in pixels.
  int height() const { return pixmap_.height(); }

  /// Access to the underlying pixmap.
  Pixmap& pixmap() { return pixmap_; }

  /// Immutable access to the underlying pixmap.
  const Pixmap& pixmap() const { return pixmap_; }

  /**
   * Fills a path and blends it into the canvas pixmap.
   *
   * @param spline Path geometry to fill.
   * @param paint Paint parameters describing how to shade the fill.
   * @param fillRule Winding rule used to interpret interior coverage.
   * @param transform Transform applied to the path before rasterization.
   * @param clipMask Optional mask multiplied against rasterized coverage before blending.
   */
  Expected<std::monostate, std::string> drawPath(const svg::PathSpline& spline, const Paint& paint,
                                                 FillRule fillRule = FillRule::kNonZero,
                                                 const Transform& transform = Transform(),
                                                 const Mask* clipMask = nullptr);

  /**
   * Strokes a path outline and blends it into the canvas pixmap.
   *
   * @param spline Path geometry to stroke.
   * @param stroke Stroke parameters describing width, joins, caps, and dashes.
   * @param paint Paint parameters describing how to shade the stroke.
   * @param transform Transform applied to the path before stroking and rasterization.
   * @param clipMask Optional mask multiplied against rasterized coverage before blending.
   */
  Expected<std::monostate, std::string> strokePath(const svg::PathSpline& spline,
                                                   const Stroke& stroke, const Paint& paint,
                                                   const Transform& transform = Transform(),
                                                   const Mask* clipMask = nullptr);

  /**
   * Draws a pixmap onto the canvas at the given position.
   */
  Expected<std::monostate, std::string> drawPixmap(int x, int y, const Pixmap& source,
                                                   const PixmapPaint& paint,
                                                   const Transform& transform = Transform(),
                                                   const Mask* clipMask = nullptr);

  /// Fills the entire canvas with the given color.
  void clear(Color color);

private:
  explicit Canvas(Pixmap pixmap);

  Pixmap pixmap_;
};

}  // namespace donner::backends::tiny_skia_cpp
