#pragma once
/// @file

#include <optional>
#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg::parser {

/**
 * @page path_data Path Data Syntax
 *
 * \details The `d` attribute of a \ref xml_path element defines the shape of the path. It is a
 * sequence of commands, each of which is a single letter followed by a sequence of numbers, such as
 * `M 40 50`. To parse the `d` attribute, use \ref PathParser::Parse.
 *
 * If the letter is uppercase, the coordinates that follow are absolute coordinates. If the letter
 * is lowercase, the coordinates are relative to the current point.
 *
 * | Command | Function | Parameters | Description |
 * | ------- | -------- | ---------- | ----------- |
 * | **M**   | \ref PathSpline::moveTo | `(x y)+` | Start a new sub-path at `(x, y)`. If additional coordinates follow, they are treated as \ref PathSpline::Builder::lineTo. |
 * | **Z**   | \ref PathSpline::closePath | | Close the current sub-path by drawing a line from the current point to the starting point of the sub-path. |
 * | **Line commands** ||||
 * | **L**   | \ref PathSpline::lineTo | `(x y)+` | Draw a line from the current point to `(x, y)`. |
 * | **H**   | Horizontal line to | `x+` | Draw a horizontal line from the current point to `(x, currentY)`. |
 * | **V**   | Vertical line to | `y+` | Draw a vertical line from the current point to `(currentX, y)`. |
 * | **Cubic Bezier curve commands** ||||
 * | **C**   | \ref PathSpline::curveTo | `(x1 y1 x2 y2 x y)+` | Draw a cubic Bezier curve from the current point to `(x, y)`, using `(x1, y1)` and `(x2, y2)` as the control points. |
 * | **S**   | Smooth curve to | `(x2 y2 x y)+` | Draw a cubic Bezier curve from the current point to `(x, y)`, using a reflection of the previous command's control point and `(x2, y2)` as the control points, creating a smooth curve. |
 * | **Quadratic Bezier curve commands** ||||
 * | **Q**   | Quadratic curve to | `(x1 y1 x y)+` | Draw a quadratic Bezier curve from the current point to `(x, y)`, using `(x1, y1)` as the control point. |
 * | **T**   | Smooth quadratic curve to | `(x y)+` | Draw a quadratic Bezier curve from the current point to `(x, y)`, using a reflection of the previous command's control point as the control point, creating a smooth curve. |
 * | **Elliptical arc commands** ||||
 * | **A**   | \ref PathSpline::arcTo | `(rx ry x-axis-rotation large-arc-flag sweep-flag x y)+` | Draw an elliptical arc from the current point to `(x, y)`, using `(rx, ry)` as the radii of the ellipse, and `x-axis-rotation` as the rotation of the ellipse. The `large-arc-flag` and `sweep-flag` parameters control the size and orientation of the arc. |
 *
 * @see https://www.w3.org/TR/SVG2/paths.html#PathData
 */

/**
 * Parse an SVG path "d"-string, see \ref path_data.
 */
class PathParser {
public:
  /**
   * Parse an SVG path "d"-string, see \ref path_data.
   *
   * @param d String corresponding to the SVG `<path d="...">` parameter.
   * @param outWarning Optional destination for a warning emitted when parsing stops early due to
   *                   non-critical errors. When provided, the warning message is captured here.
   *
   * Error handling:
   * - **Critical errors** (e.g., invalid commands, malformed path data with no parseable content):
   *   Returns ParseError with no result.
   * - **Non-critical errors** (e.g., trailing invalid data after valid path commands): Returns
   *   partial result. If `outWarning` is provided, the error details are captured there.
   *
   * @return Parsed PathSpline or an error.
   */
  static ParseResult<PathSpline> Parse(std::string_view d,
                                       std::optional<ParseError>* outWarning = nullptr);
};

}  // namespace donner::svg::parser
