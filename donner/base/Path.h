#pragma once
/// @file

#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

#include "donner/base/BezierUtils.h"
#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner {

/// Line cap style for stroke endpoints.
enum class LineCap : uint8_t {
  Butt,    ///< The stroke is squared off at the endpoint of the path.
  Round,   ///< The stroke is rounded at the endpoint of the path.
  Square,  ///< The stroke extends beyond the endpoint by half the stroke width and is squared off.
};

/// Ostream output operator for \ref LineCap.
inline std::ostream& operator<<(std::ostream& os, LineCap cap) {
  switch (cap) {
    case LineCap::Butt: return os << "Butt";
    case LineCap::Round: return os << "Round";
    case LineCap::Square: return os << "Square";
  }
  return os << "Unknown";
}

/// Line join style for stroke corners.
enum class LineJoin : uint8_t {
  Miter,  ///< The outer edges of the strokes are extended until they meet at a sharp point.
  Round,  ///< The corners of the stroke are rounded using a circular arc.
  Bevel,  ///< A triangular shape fills the area between the two stroked segments.
};

/// Ostream output operator for \ref LineJoin.
inline std::ostream& operator<<(std::ostream& os, LineJoin join) {
  switch (join) {
    case LineJoin::Miter: return os << "Miter";
    case LineJoin::Round: return os << "Round";
    case LineJoin::Bevel: return os << "Bevel";
  }
  return os << "Unknown";
}

/// Parameters for converting a stroked path to a filled outline.
struct StrokeStyle {
  double width = 1.0;          ///< Stroke width.
  LineCap cap = LineCap::Butt;     ///< Line cap style for open subpath endpoints.
  LineJoin join = LineJoin::Miter;  ///< Line join style for corners between segments.
  double miterLimit = 4.0;     ///< Maximum miter length ratio (as per SVG stroke-miterlimit).

  /// Dash pattern lengths alternating on/off segments (SVG `stroke-dasharray`).
  /// Empty vector means solid stroke. If the vector has an odd number of
  /// entries, the pattern is implicitly doubled per the SVG spec.
  std::vector<double> dashArray;

  /// Initial phase offset along the dash pattern (SVG `stroke-dashoffset`).
  /// Positive values shift the dash pattern forward; negative values shift it
  /// backward. The phase wraps modulo the pattern length.
  double dashOffset = 0.0;

  /// SVG `pathLength` attribute. When non-zero, dasharray values and dashoffset
  /// are interpreted relative to this length rather than the actual path
  /// length, so all dash distances are scaled by `actualLength / pathLength`.
  /// Zero (the default) means use the actual path length unchanged.
  double pathLength = 0.0;
};

class PathBuilder;

/**
 * Immutable 2D vector path.
 *
 * Once constructed via \ref PathBuilder, a Path is immutable and thread-safe. Paths are suitable
 * for caching in ECS components (e.g., GPU band data for the Geode renderer).
 *
 * Supports line segments, quadratic Bézier curves, and cubic Bézier curves.
 */
class Path {
public:
  /// Verb types describing how points are connected.
  enum class Verb : uint8_t {
    MoveTo,     ///< Start a new subpath. Consumes 1 point.
    LineTo,     ///< Straight line to a point. Consumes 1 point.
    QuadTo,     ///< Quadratic Bézier curve. Consumes 2 points (control, end).
    CurveTo,    ///< Cubic Bézier curve. Consumes 3 points (c1, c2, end).
    ClosePath,  ///< Close the current subpath. Consumes 0 points.
  };

  /// Ostream output operator for Verb.
  friend std::ostream& operator<<(std::ostream& os, Verb verb);

  /// A command in the path, pairing a verb with the index of its first point.
  struct Command {
    Verb verb;              ///< The verb type.
    uint32_t pointIndex;    ///< Index of the first point for this command in the points array.
    bool isInternal = false; ///< True for intermediate segments of arc decomposition. These are
                             ///< skipped when computing vertices for marker placement.

    /// Equality operator.
    bool operator==(const Command& other) const {
      return verb == other.verb && pointIndex == other.pointIndex;
    }

    /// Ostream output operator.
    friend std::ostream& operator<<(std::ostream& os, const Command& command);
  };

  /// Construct an empty path.
  Path() = default;

  // Copyable and movable.
  /// Copy constructor.
  Path(const Path&) = default;
  /// Move constructor.
  Path(Path&&) noexcept = default;
  /// Copy assignment operator.
  Path& operator=(const Path&) = default;
  /// Move assignment operator.
  Path& operator=(Path&&) noexcept = default;
  /// Destructor.
  ~Path() = default;

  /// @name Accessors
  /// @{

  /// Returns the points array.
  std::span<const Vector2d> points() const { return points_; }

  /// Returns the commands array.
  std::span<const Command> commands() const { return commands_; }

  /// Returns true if the path has no commands.
  bool empty() const { return commands_.empty(); }

  /// Returns the number of commands (verbs) in the path.
  size_t verbCount() const { return commands_.size(); }

  /// @}

  /// @name Geometric queries
  /// @{

  /// Returns the axis-aligned bounding box of the path.
  Box2d bounds() const;

  /// Returns the bounding box of the path transformed by \p transform.
  Box2d transformedBounds(const Transform2d& transform) const;

  /**
   * Returns true if the given point is inside this path's fill region.
   *
   * Uses a winding-number ray-casting algorithm. Points lying exactly on the path boundary are
   * also considered inside.
   *
   * @param point Point to test.
   * @param fillRule Fill rule to use (NonZero or EvenOdd).
   */
  bool isInside(const Vector2d& point, FillRule fillRule = FillRule::NonZero) const;

  /**
   * Returns true if the given point is within \p strokeWidth / 2 of any path segment.
   *
   * @param point Point to test.
   * @param strokeWidth Width of the stroke.
   */
  bool isOnPath(const Vector2d& point, double strokeWidth) const;

  /// Result of sampling the path at a given arc length distance.
  struct PointOnPath {
    Vector2d point;     ///< Position on the path.
    Vector2d tangent;   ///< Un-normalized tangent vector at the point.
    double angle;       ///< Tangent angle in radians (atan2).
    bool valid = true;  ///< False if distance exceeds path length.
  };

  /// Compute the total arc length of the path.
  ///
  /// Line segments are measured directly. Quadratic Bezier curves are elevated to cubics.
  /// Cubic Bezier arc lengths are approximated via recursive subdivision.
  double pathLength() const;

  /// Sample the path at the given arc length \p distance from the start.
  ///
  /// Returns the position, tangent vector, and tangent angle at that distance. If \p distance
  /// exceeds the total path length, returns the endpoint with \c valid = false.
  PointOnPath pointAtArcLength(double distance) const;

  /**
   * Evaluate the position on segment \p index at parameter \p t in [0, 1].
   *
   * - MoveTo: returns the point.
   * - LineTo / ClosePath: linear interpolation.
   * - QuadTo: De Casteljau quadratic evaluation.
   * - CurveTo: De Casteljau cubic evaluation.
   *
   * @param index Command index.
   * @param t Parameter in [0, 1].
   */
  Vector2d pointAt(size_t index, double t) const;

  /**
   * Return the un-normalized tangent vector at segment \p index, parameter \p t.
   *
   * The tangent is the first derivative of the curve at \p t. For degenerate cubic curves where
   * the derivative is zero, the parameter is slightly adjusted and retried.
   *
   * - MoveTo: forwards to the next segment's tangent at t=0.
   * - LineTo / ClosePath: constant direction (endpoint - startPoint).
   * - QuadTo: derivative of the quadratic Bézier.
   * - CurveTo: derivative of the cubic Bézier.
   *
   * @param index Command index.
   * @param t Parameter in [0, 1].
   */
  Vector2d tangentAt(size_t index, double t) const;

  /**
   * Return the normal vector (perpendicular to the tangent) at segment \p index, parameter \p t.
   *
   * Computed as the 90-degree counter-clockwise rotation of the normalized tangent.
   *
   * @param index Command index.
   * @param t Parameter in [0, 1].
   */
  Vector2d normalAt(size_t index, double t) const;

  /**
   * Compute the bounding box of the stroked path, accounting for miter joins.
   *
   * At each join between segments, the miter extension is computed and included in the bounding
   * box when it does not exceed \p miterLimit.
   *
   * @param strokeWidth Width of the stroke.
   * @param miterLimit Maximum miter length (as per SVG stroke-miterlimit).
   * @return Bounding box including stroke and miter extensions.
   */
  Box2d strokeMiterBounds(double strokeWidth, double miterLimit) const;

  /// @}

  /// @name Conversions
  /// @{

  /**
   * Convert all cubic Bézier curves to quadratic approximations within \p tolerance.
   *
   * This is critical for the Slug GPU rendering pipeline, where quadratic root-finding in the
   * fragment shader is significantly cheaper than cubic.
   *
   * @param tolerance Maximum allowed distance between the cubic and its quadratic approximation.
   *                  Default 0.1 is suitable for text-size content.
   * @return A new Path containing only MoveTo, LineTo, QuadTo, and ClosePath verbs.
   */
  Path cubicToQuadratic(double tolerance = 0.1) const;

  /**
   * Split all curves at Y-extrema so each segment is monotonic in Y.
   *
   * Required for Slug band decomposition — a monotonic curve intersects any horizontal band
   * boundary at most once.
   *
   * @return A new Path where every QuadTo and CurveTo segment is Y-monotonic.
   */
  Path toMonotonic() const;

  /**
   * Flatten all curves to line segments within \p tolerance.
   *
   * @param tolerance Maximum distance between the curve and its line approximation.
   * @return A new Path containing only MoveTo, LineTo, and ClosePath verbs.
   */
  Path flatten(double tolerance = 0.25) const;

  /**
   * Convert this path's stroke to a filled outline.
   *
   * Takes stroke parameters and returns a new closed Path whose fill region represents the area
   * that would be covered by stroking this path with the given style.
   *
   * The implementation flattens curves to line segments first, then offsets each segment by
   * `width/2` perpendicular to the segment direction, applying the specified line join at corners
   * and line cap at open subpath endpoints.
   *
   * @param style Stroke parameters (width, cap, join, miter limit).
   * @param flattenTolerance Tolerance for curve flattening.
   * @return A new Path representing the filled outline of the stroke.
   */
  Path strokeToFill(const StrokeStyle& style, double flattenTolerance = 0.25) const;

  /// @}

  /// @name Serialization
  /// @{

  /**
   * Serialize this path to its canonical SVG `d` attribute text.
   *
   * Commands are emitted using absolute uppercase letters with a single space
   * between parameters.  Integers are emitted without a decimal point; fractional
   * values use the shortest round-trippable decimal form (C++20 `{}` format).
   *
   * Supported verbs and output format:
   * - MoveTo    → `M x y`
   * - LineTo    → `L x y`
   * - QuadTo    → `Q cx cy x y`
   * - CurveTo   → `C c1x c1y c2x c2y x y`
   * - ClosePath → `Z`
   *
   * Arc commands (`PathBuilder::arcTo`) are decomposed to cubic Bézier curves
   * by the builder before they reach the Path data model, so they appear as
   * one or more `C` segments here.
   *
   * Round-trips with `donner::svg::parser::PathParser::Parse`.
   *
   * @return SVG path `d` string, empty string for an empty path.
   */
  RcString toSVGPathData() const;

  /// @}

  /// @name Iteration
  /// @{

  /**
   * Iterate over path segments, calling \p fn for each command.
   *
   * The callback receives the verb and a span of the points consumed by that verb:
   * - MoveTo: 1 point (destination)
   * - LineTo: 1 point (destination)
   * - QuadTo: 2 points (control, end)
   * - CurveTo: 3 points (c1, c2, end)
   * - ClosePath: 0 points
   *
   * @param fn Callback invoked for each command.
   */
  template <typename F>
  void forEach(F&& fn) const {
    for (const auto& cmd : commands_) {
      const size_t n = pointsPerVerb(cmd.verb);
      fn(cmd.verb, std::span<const Vector2d>(points_.data() + cmd.pointIndex, n));
    }
  }

  /// Vertex in a path's edge list, used for marker placement.
  struct Vertex {
    Vector2d point;        ///< Point on the path.
    Vector2d orientation;  ///< Orientation of the path at the point, normalized.

    /// Ostream operator for Vertex, which outputs a human-readable representation.
    friend std::ostream& operator<<(std::ostream& os, const Vertex& vertex) {
      return os << "Vertex(point=" << vertex.point << ", orientation=" << vertex.orientation << ")";
    }
  };

  /// Returns the vertices of the path as a flat list for marker placement.
  ///
  /// Each vertex contains a point on the path and an orientation vector indicating the direction
  /// at that point. For vertices at joints between segments, the orientation is interpolated
  /// between the outgoing tangent of the previous segment and the incoming tangent of the next.
  std::vector<Vertex> vertices() const;

  /// @}

  /// Returns the number of points consumed by a given verb.
  static constexpr size_t pointsPerVerb(Verb verb) {
    switch (verb) {
      case Verb::MoveTo: return 1;
      case Verb::LineTo: return 1;
      case Verb::QuadTo: return 2;
      case Verb::CurveTo: return 3;
      case Verb::ClosePath: return 0;
    }
    return 0;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Path& path);

private:
  friend class PathBuilder;

  std::vector<Vector2d> points_;
  std::vector<Command> commands_;
};

/**
 * Mutable builder for constructing immutable \ref Path objects.
 *
 * Usage:
 * ```
 * Path path = PathBuilder()
 *     .moveTo({0, 0})
 *     .lineTo({100, 0})
 *     .quadTo({100, 100}, {0, 100})
 *     .closePath()
 *     .build();
 * ```
 */
class PathBuilder {
public:
  PathBuilder() = default;

  /// @name Path construction
  /// @{

  /// Start a new subpath at \p point.
  PathBuilder& moveTo(const Vector2d& point);

  /// Line from the current point to \p point.
  PathBuilder& lineTo(const Vector2d& point);

  /// Quadratic Bézier from the current point through \p control to \p end.
  PathBuilder& quadTo(const Vector2d& control, const Vector2d& end);

  /// Cubic Bézier from the current point through \p c1, \p c2 to \p end.
  PathBuilder& curveTo(const Vector2d& c1, const Vector2d& c2, const Vector2d& end);

  /**
   * Elliptical arc from the current point to \p end.
   *
   * The arc is decomposed into cubic Bézier curves internally.
   *
   * @param radius Ellipse radius (before rotation).
   * @param rotationRadians X-axis rotation of the ellipse in radians.
   * @param largeArc True for arc length >= 180 degrees.
   * @param sweep True for positive-angle arc direction.
   * @param end End point of the arc.
   */
  PathBuilder& arcTo(const Vector2d& radius, double rotationRadians, bool largeArc, bool sweep,
                     const Vector2d& end);

  /// Close the current subpath with a straight line back to the last moveTo point.
  PathBuilder& closePath();

  /// @}

  /// @name Shape helpers
  /// @{

  /// Add an axis-aligned rectangle.
  PathBuilder& addRect(const Box2d& rect);

  /// Add a rounded rectangle with corner radii \p rx and \p ry.
  PathBuilder& addRoundedRect(const Box2d& rect, double rx, double ry);

  /// Add an ellipse inscribed in \p bounds.
  PathBuilder& addEllipse(const Box2d& bounds);

  /// Add a circle with center \p center and radius \p radius.
  PathBuilder& addCircle(const Vector2d& center, double radius);

  /// Append all commands from \p path.
  PathBuilder& addPath(const Path& path);

  /// @}

  /// @name State
  /// @{

  /// Returns the current point (end point of the last command), or (0,0) if empty.
  Vector2d currentPoint() const;

  /// Returns true if no commands have been added.
  bool empty() const { return path_.commands_.empty(); }

  /// @}

  /// Build the immutable Path. The builder is reset after this call.
  Path build();

private:
  void ensureMoveTo();

  Path path_;
  Vector2d lastMoveTo_;
  uint32_t moveToPointIndex_ = 0;
  bool hasMoveTo_ = false;
};

}  // namespace donner
