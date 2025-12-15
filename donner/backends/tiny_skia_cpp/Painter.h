#pragma once
/// @file

#include <string>
#include <variant>

#include "donner/backends/tiny_skia_cpp/Expected.h"
#include "donner/backends/tiny_skia_cpp/Mask.h"
#include "donner/backends/tiny_skia_cpp/Paint.h"
#include "donner/backends/tiny_skia_cpp/Pixmap.h"
#include "donner/backends/tiny_skia_cpp/Rasterizer.h"
#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::backends::tiny_skia_cpp {

/**
 * Rasterizes a path into a mask and blends it into a destination pixmap.
 *
 * @param spline Path geometry to fill.
 * @param paint Paint parameters describing how to shade the fill.
 * @param pixmap Destination surface that receives blended pixels.
 * @param clipMask Optional mask that is multiplied against rasterized coverage before blending.
 */
Expected<std::monostate, std::string> FillPath(const svg::PathSpline& spline, const Paint& paint,
                                               Pixmap& pixmap,
                                               FillRule fillRule = FillRule::kNonZero,
                                               const Transform& transform = Transform(),
                                               const Mask* clipMask = nullptr);

/**
 * Rasterizes a stroked path outline into a mask and blends it into a destination pixmap.
 *
 * @param spline Path geometry to stroke.
 * @param stroke Stroke parameters describing width, joins, caps, and dashes.
 * @param paint Paint parameters describing how to shade the stroke.
 * @param pixmap Destination surface that receives blended pixels.
 * @param clipMask Optional mask multiplied against rasterized coverage before blending.
 */
Expected<std::monostate, std::string> StrokePath(const svg::PathSpline& spline, const Stroke& stroke,
                                                 const Paint& paint, Pixmap& pixmap,
                                                 const Transform& transform = Transform(),
                                                 const Mask* clipMask = nullptr);

/**
 * Draws a pixmap onto the destination surface using pattern sampling.
 */
Expected<std::monostate, std::string> DrawPixmap(int x, int y, const Pixmap& source,
                                                 const PixmapPaint& paint, Pixmap& pixmap,
                                                 const Transform& transform = Transform(),
                                                 const Mask* clipMask = nullptr);

}  // namespace donner::backends::tiny_skia_cpp
