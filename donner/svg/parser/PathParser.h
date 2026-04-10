#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/Path.h"

namespace donner::svg::parser {

/**
 * @page path_data Path Data Syntax
 *
 * \tableofcontents
 *
 * \details The `d` attribute of a \ref xml_path element defines the shape of the path as a
 * sequence of commands, each a single letter followed by numeric parameters (e.g., `M 40 50`).
 * To parse a `d` attribute in code, use \ref PathParser::Parse.
 *
 * A path is a sequence of **sub-paths**, each starting with a `M` (moveto) and optionally
 * ending with a `Z` (closepath). Between them, line, curve, and arc commands draw shapes
 * relative to the **current point**, which advances after every drawing command.
 *
 * **Absolute vs. relative:** Commands use uppercase letters for absolute coordinates and
 * lowercase for coordinates relative to the current point. For example, `L 100 50` draws to
 * the point `(100, 50)` in user space, while `l 10 5` draws to a point 10 units right and 5
 * units down from wherever the current point is.
 *
 * **Compact form:** Whitespace and commas are interchangeable, and repeated command letters
 * may be omitted. The following are all equivalent:
 * - `M 40 50 L 80 90 L 120 90`
 * - `M40,50 L80,90 120,90`
 * - `M40 50 L80 90,120 90`
 *
 * @see https://www.w3.org/TR/SVG2/paths.html#PathData
 *
 * @section path_data_moveto Moveto and closepath
 *
 * `M` starts a new sub-path at the given point without drawing anything, and `Z` closes the
 * current sub-path by drawing a straight line back to its starting point. Every path must
 * begin with an `M` command.
 *
 * | Command | Function | Parameters | Description |
 * | :-----: | -------- | ---------- | ----------- |
 * | **M**   | \ref PathBuilder::moveTo | `(x y)+` | Start a new sub-path at `(x, y)`. If additional coordinate pairs follow, they are treated as implicit `L` commands. |
 * | **Z**   | \ref PathBuilder::closePath | (none) | Close the current sub-path by drawing a line from the current point to the starting point of the sub-path. |
 *
 * \htmlonly
 * <svg id="path_data_moveto" width="320" height="180" viewBox="0 0 320 180" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <style>
 *       #path_data_moveto .sample { fill: none; stroke: #1f5a8a; stroke-width: 2.5; }
 *       #path_data_moveto .close  { fill: none; stroke: #c33;    stroke-width: 2; stroke-dasharray: 5,3; }
 *       #path_data_moveto .dot    { fill: #c33; }
 *     </style>
 *   </defs>
 *   <path class="sample" d="M 40 130 L 140 40 L 230 130" />
 *   <path class="close"  d="M 40 130 L 230 130" />
 *   <circle class="dot" cx="40"  cy="130" r="4" />
 *   <circle class="dot" cx="140" cy="40"  r="4" />
 *   <circle class="dot" cx="230" cy="130" r="4" />
 *   <text x="10"  y="148">M 40 130</text>
 *   <text x="125" y="28">L 140 40</text>
 *   <text x="200" y="148">L 230 130</text>
 *   <text x="120" y="170" fill="#c33">Z closes back to M</text>
 * </svg>
 * \endhtmlonly
 *
 * @section path_data_lines Line commands
 *
 * Line commands draw straight line segments from the current point to a new point.
 *
 * | Command | Function | Parameters | Description |
 * | :-----: | -------- | ---------- | ----------- |
 * | **L**   | \ref PathBuilder::lineTo    | `(x y)+` | Draw a line to `(x, y)`. |
 * | **H**   | Horizontal line to   | `x+`     | Draw a horizontal line to `(x, currentY)`. |
 * | **V**   | Vertical line to     | `y+`     | Draw a vertical line to `(currentX, y)`. |
 *
 * \htmlonly
 * <svg id="path_data_lines" width="320" height="180" viewBox="0 0 320 180" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <style>
 *       #path_data_lines .sample { fill: none; stroke: #1f5a8a; stroke-width: 2.5; }
 *       #path_data_lines .dot    { fill: #c33; }
 *       #path_data_lines text.cmd { fill: #333; font-family: monospace; }
 *     </style>
 *   </defs>
 *   <path class="sample" d="M 30 40 L 110 40 H 190 V 120 L 270 150" />
 *   <circle class="dot" cx="30"  cy="40"  r="4" />
 *   <circle class="dot" cx="110" cy="40"  r="4" />
 *   <circle class="dot" cx="190" cy="40"  r="4" />
 *   <circle class="dot" cx="190" cy="120" r="4" />
 *   <circle class="dot" cx="270" cy="150" r="4" />
 *   <text class="cmd" x="4"   y="35">M 30 40</text>
 *   <text class="cmd" x="30"  y="58">L 110 40</text>
 *   <text class="cmd" x="130" y="58">H 190</text>
 *   <text class="cmd" x="196" y="85">V 120</text>
 *   <text class="cmd" x="200" y="145">L 270 150</text>
 * </svg>
 * \endhtmlonly
 *
 * @section path_data_cubic Cubic Bézier curves
 *
 * Cubic Bézier commands draw a smooth curve to a new point using **two control points** that
 * pull the curve tangent at each endpoint.
 *
 * | Command | Function | Parameters | Description |
 * | :-----: | -------- | ---------- | ----------- |
 * | **C**   | \ref PathBuilder::curveTo | `(x1 y1 x2 y2 x y)+` | Draw a cubic Bézier from the current point to `(x, y)` with control points `(x1, y1)` (for the start) and `(x2, y2)` (for the end). |
 * | **S**   | Smooth cubic curve to | `(x2 y2 x y)+` | Same as `C`, but the first control point is implicitly the **reflection** of the previous command's second control point across the current point. Chains after a `C` or another `S`. |
 *
 * \htmlonly
 * <svg id="path_data_cubic" width="320" height="180" viewBox="0 0 320 180" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <style>
 *       #path_data_cubic .sample  { fill: none; stroke: #1f5a8a; stroke-width: 2.5; }
 *       #path_data_cubic .handle  { fill: none; stroke: #c33;    stroke-width: 1.5; stroke-dasharray: 4,3; }
 *       #path_data_cubic .handle2 { fill: none; stroke: #2a9;    stroke-width: 1.5; stroke-dasharray: 4,3; }
 *       #path_data_cubic .anchor  { fill: #c33; }
 *       #path_data_cubic .ctrl1   { fill: #c33; }
 *       #path_data_cubic .ctrl2   { fill: #2a9; }
 *     </style>
 *   </defs>
 *   <path class="sample" d="M 40 140 C 80 30 180 30 220 140 S 300 30 300 30" />
 *   <line class="handle"  x1="40"  y1="140" x2="80"  y2="30" />
 *   <line class="handle"  x1="220" y1="140" x2="180" y2="30" />
 *   <line class="handle2" x1="220" y1="140" x2="260" y2="250" />
 *   <line class="handle2" x1="300" y1="30"  x2="300" y2="30" />
 *   <circle class="anchor" cx="40"  cy="140" r="4" />
 *   <circle class="ctrl1"  cx="80"  cy="30"  r="3.5" />
 *   <circle class="ctrl1"  cx="180" cy="30"  r="3.5" />
 *   <circle class="anchor" cx="220" cy="140" r="4" />
 *   <circle class="anchor" cx="300" cy="30"  r="4" />
 *   <text x="4"   y="158">M 40 140</text>
 *   <text x="60"  y="22">C ctrl1</text>
 *   <text x="150" y="22">ctrl2</text>
 *   <text x="190" y="158">→ 220 140</text>
 *   <text x="240" y="158" fill="#2a9">S (reflect)</text>
 * </svg>
 * \endhtmlonly
 *
 * The dashed red handles are the explicit control points for the first `C` command. The `S`
 * that follows reuses a **reflected** first control point automatically (shown in teal) to
 * continue the curve with C¹ continuity.
 *
 * @section path_data_quadratic Quadratic Bézier curves
 *
 * Quadratic Bézier commands use a **single control point** shared by both endpoints. Use
 * them when cubic curves are more precision than you need.
 *
 * | Command | Function | Parameters | Description |
 * | :-----: | -------- | ---------- | ----------- |
 * | **Q**   | Quadratic curve to | `(x1 y1 x y)+` | Draw a quadratic Bézier from the current point to `(x, y)` with control point `(x1, y1)`. |
 * | **T**   | Smooth quadratic curve to | `(x y)+` | Same as `Q`, but the control point is implicitly the **reflection** of the previous command's control point across the current point. Chains after a `Q` or another `T`. |
 *
 * \htmlonly
 * <svg id="path_data_quadratic" width="320" height="180" viewBox="0 0 320 180" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <style>
 *       #path_data_quadratic .sample { fill: none; stroke: #1f5a8a; stroke-width: 2.5; }
 *       #path_data_quadratic .handle { fill: none; stroke: #c33;    stroke-width: 1.5; stroke-dasharray: 4,3; }
 *       #path_data_quadratic .anchor { fill: #c33; }
 *       #path_data_quadratic .ctrl   { fill: #c33; }
 *     </style>
 *   </defs>
 *   <path class="sample" d="M 40 140 Q 120 20 200 140 T 360 140" />
 *   <line class="handle" x1="40"  y1="140" x2="120" y2="20" />
 *   <line class="handle" x1="200" y1="140" x2="120" y2="20" />
 *   <circle class="anchor" cx="40"  cy="140" r="4" />
 *   <circle class="ctrl"   cx="120" cy="20"  r="3.5" />
 *   <circle class="anchor" cx="200" cy="140" r="4" />
 *   <text x="4"   y="158">M 40 140</text>
 *   <text x="100" y="15">Q ctrl</text>
 *   <text x="165" y="158">→ 200 140</text>
 *   <text x="235" y="158" fill="#333">T 360 140</text>
 * </svg>
 * \endhtmlonly
 *
 * @section path_data_arc Elliptical arc
 *
 * The `A` command draws a portion of an **ellipse** between the current point and a new
 * endpoint. Four parameters disambiguate which of the (up to) four possible ellipse arcs
 * to draw:
 *
 * | Command | Parameters | Description |
 * | :-----: | ---------- | ----------- |
 * | **A**   | `rx ry x-axis-rotation large-arc-flag sweep-flag x y` | Draw an elliptical arc to `(x, y)` using radii `rx` and `ry`, rotated by `x-axis-rotation` degrees. `large-arc-flag` chooses the longer (`1`) or shorter (`0`) of the two possible arcs, and `sweep-flag` chooses the arc going clockwise (`1`) or counter-clockwise (`0`). See \ref PathBuilder::arcTo. |
 *
 * The four flag combinations pick out the four distinct arcs between the same pair of
 * endpoints on the same ellipse:
 *
 * \htmlonly
 * <svg id="path_data_arc" width="320" height="200" viewBox="0 0 320 200" style="background-color: white" font-family="sans-serif" font-size="11">
 *   <defs>
 *     <style>
 *       #path_data_arc path.sample { fill: none; stroke-width: 2.5; }
 *       #path_data_arc path.arc00  { stroke: #1f5a8a; }
 *       #path_data_arc path.arc01  { stroke: #2a9;    }
 *       #path_data_arc path.arc10  { stroke: #c33;    }
 *       #path_data_arc path.arc11  { stroke: #a6a;    }
 *       #path_data_arc .endpoint   { fill: #333; }
 *       #path_data_arc text.label  { fill: #333; font-family: monospace; }
 *     </style>
 *   </defs>
 *   <path class="sample arc00" d="M 80 150 A 60 40 0 0 0 220 150" />
 *   <path class="sample arc01" d="M 80 150 A 60 40 0 0 1 220 150" />
 *   <path class="sample arc10" d="M 80 150 A 60 40 0 1 0 220 150" />
 *   <path class="sample arc11" d="M 80 150 A 60 40 0 1 1 220 150" />
 *   <circle class="endpoint" cx="80"  cy="150" r="4" />
 *   <circle class="endpoint" cx="220" cy="150" r="4" />
 *   <text x="30"  y="170">M 80 150</text>
 *   <text x="200" y="170">→ 220 150</text>
 *   <text class="label" x="8"   y="22" fill="#1f5a8a">A 60 40 0 0 0</text>
 *   <text class="label" x="8"   y="40" fill="#2a9">A 60 40 0 0 1</text>
 *   <text class="label" x="195" y="22" fill="#c33">A 60 40 0 1 0</text>
 *   <text class="label" x="195" y="40" fill="#a6a">A 60 40 0 1 1</text>
 * </svg>
 * \endhtmlonly
 *
 * All four curves use `rx=60 ry=40 x-axis-rotation=0`, but different `large-arc-flag` /
 * `sweep-flag` combinations produce the four distinct arcs between the same endpoints.
 */

/**
 * Parse an SVG path "d"-string, see \ref path_data.
 */
class PathParser {
public:
  /**
   * Parse an SVG path "d"-string, see \ref path_data.
   *
   * Note that this parser may return both an error and a partial path, since path parsing will
   * return anything that it has parsed before it encountered the error. The caller should use both
   * `ParseResult::hasResult()` and `ParseResult::hasError()` to determine what has been returned.
   *
   * @param d String corresponding to the SVG `<path d="...">` parameter.
   * @return Parsed Path and/or an error.
   */
  static ParseResult<Path> Parse(std::string_view d);
};

}  // namespace donner::svg::parser
