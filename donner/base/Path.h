#pragma once
/// @file

#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

#include "donner/base/BezierUtils.h"
#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner {

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

    /// Equality operator.
    bool operator==(const Command& other) const {
      return verb == other.verb && pointIndex == other.pointIndex;
    }
  };

  /// Construct an empty path.
  Path() = default;

  // Copyable and movable.
  Path(const Path&) = default;
  Path(Path&&) noexcept = default;
  Path& operator=(const Path&) = default;
  Path& operator=(Path&&) noexcept = default;
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
  bool hasMoveTo_ = false;
};

}  // namespace donner
