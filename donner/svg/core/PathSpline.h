#pragma once
/// @file

#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/core/FillRule.h"

namespace donner::svg {

/**
 * Container for a spline, which is a series of points connected by lines and curves.
 *
 * This is used to represent the `d` attribute of the \ref SVGPathElement (\ref xml_path), see
 * https://www.w3.org/TR/SVG2/paths.html#PathData. To parse SVG path data into a PathSpline, use the
 * \ref donner::svg::parser::PathParser.
 *
 * The spline is composed of a series of \ref CommandType commands, which describe how to connect
 * the points. The points are stored in a separate vector, and the commands reference the points by
 * index.
 */
class PathSpline {
public:
  /**
   * Type of command to connect the points.
   *
   * Note that these may not map 1:1 to the SVG path commands, as the commands are decomposed into
   * simpler curves.
   */
  enum class CommandType {
    /**
     * Move the starting point of the spline to a new point, creating a new subpath.
     *
     * Consumes 1 point:
     * - 1: New starting point.
     */
    MoveTo,

    /**
     * Draw a line from the current point to a new point.
     *
     * Consumes 1 point:
     * - 1: End point of the line.
     */
    LineTo,

    /**
     * Draw a cubic Bézier curve from the current point to a new point.
     *
     * Consumes 3 points:
     * - 1: First control point.
     * - 2: Second control point.
     * - 3: End point of the curve.
     */
    CurveTo,

    /**
     * Close the path.
     *
     * An automatic straight line is drawn from the current point back to the initial point of
     * the current subpath.
     *
     * Consumes 0 points.
     */
    ClosePath
  };

  /**
   * Ostream operator for CommandType, e.g. `CommandType::MoveTo`.
   *
   * @param os Output stream.
   * @param type Command type.
   */
  friend std::ostream& operator<<(std::ostream& os, CommandType type);

  /**
   * Metadata for a command, which describes how to connect the points.
   */
  struct Command {
    CommandType type;   ///< Type of command.
    size_t pointIndex;  ///< Index of the first point of this command.

    /// True if the point is derived from an arc and does not represent an original user command.
    /// Used to determine if markers should be placed on the point.
    bool isInternalPoint = false;

    /// If \ref type is \ref CommandType::MoveTo, this is the index of the ClosePath at the end of
    /// the path.
    size_t closePathIndex = size_t(-1);

    /**
     * Command constructor.
     *
     * @param type Type of command.
     * @param pointIndex Index of the first point of this command.
     * @param isInternalPoint True if the point is derived from an arc and does not represent an
     * original user command. Defaults to false.
     */
    Command(CommandType type, size_t pointIndex, bool isInternalPoint = false)
        : type(type), pointIndex(pointIndex), isInternalPoint(isInternalPoint) {}

    /// Equality operator.
    friend inline bool operator==(const Command& lhs, const Command& rhs) {
      return lhs.pointIndex == rhs.pointIndex && lhs.type == rhs.type;
    }

    /**
     * Ostream operator for Command, which outputs a human-readable representation.
     *
     * @param os Output stream.
     * @param command Command to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Command& command);
  };

  /**
   * Vertex of the path, including the orientation. Used to place markers for \ref xml_marker.
   */
  struct Vertex {
    Vector2d point;        ///< Point on the path.
    Vector2d orientation;  ///< Orientation of the path at the point, normalized.

    /// Ostream operator for Vertex, which outputs a human-readable representation.
    friend std::ostream& operator<<(std::ostream& os, const Vertex& vertex) {
      return os << "Vertex(point=" << vertex.point << ", orientation=" << vertex.orientation << ")";
    }
  };

  /**
   * Construct a new empty PathSpline.
   */
  PathSpline() = default;

  /// @name Modification
  /// @{

  /**
   * Move the starting point of the spline to a new point, creating a new subpath. If this is
   * called multiple times in a row, subsequent calls will replace the previous.
   *
   * @param point Point to move to.
   */
  void moveTo(const Vector2d& point);

  /**
   * Draw a line from the current point to a new point.
   *
   * @param point End point of the line.
   */
  void lineTo(const Vector2d& point);

  /**
   * Draw a cubic Bézier curve from the current point to a new point.
   *
   * @param control1 First control point.
   * @param control2 Second control point.
   * @param endPoint End point of the curve.
   */
  void curveTo(const Vector2d& control1, const Vector2d& control2, const Vector2d& endPoint);

  /**
   * Add an elliptical arc to the path.
   *
   * @param radius Radius before rotation.
   * @param rotationRadians Rotation of the x-axis of the ellipse.
   * @param largeArcFlag False for arc length ≤ 180°, true for arc length ≥ 180°.
   * @param sweepFlag False for negative angle, true for positive angle.
   * @param endPoint End point of the arc.
   */
  void arcTo(const Vector2d& radius, double rotationRadians, bool largeArcFlag, bool sweepFlag,
             const Vector2d& endPoint);

  /**
   * Close the path.
   *
   * An automatic straight line is drawn from the current point back to the initial point of
   * the current subpath.
   */
  void closePath();

  //
  // Complex drawing.
  //

  /**
   * Draw an ellipse (uses multiple curve segments).
   *
   * @param center Center of the ellipse.
   * @param radius Ellipse radius, for both the x and y axis.
   */
  void ellipse(const Vector2d& center, const Vector2d& radius);

  /**
   * Draw a circle (uses multiple curve segments).
   *
   * @param center Center of the circle.
   * @param radius Radius.
   */
  void circle(const Vector2d& center, double radius);

  /**
   * Append an existing spline to this spline, joining the two splines together. This will
   * ignore the moveTo comand at the start of \p spline.
   *
   * @param spline Spline to append.
   * @param asInternalPath True if the spline should be treated as an internal path, which
   * means that markers will not be rendered onto its segments.
   */
  void appendJoin(const PathSpline& spline, bool asInternalPath = false);

  /** @} */

  /**
   * Returns true if the spline is empty.
   */
  bool empty() const { return commands_.empty(); }

  /**
   * Returns the points in the spline.
   */
  const std::vector<Vector2d>& points() const { return points_; }

  /**
   * Returns the commands in the spline.
   */
  const std::vector<Command>& commands() const { return commands_; }

  /**
   * Returns the number of commands in the spline.
   */
  size_t size() const { return commands_.size(); }

  /**
   * Returns the length of the spline.
   */
  double pathLength() const;

  /**
   * Get the end point of the path, where new draw commands will originate.
   */
  Vector2d currentPoint() const;

  /**
   * Returns the bounding box for this spline in local space.
   */
  Boxd bounds() const;

  /**
   * Returns the tight bounding box for this spline transformed to a target coordinate system.
   *
   * @param pathFromTarget Transform to transform the path to the target coordinate system.
   */
  Boxd transformedBounds(const Transformd& pathFromTarget) const;

  /**
   * Get the bounds of critical points created by miter joints when applying a stroke to this
   * path.
   *
   * @param strokeWidth Width of stroke.
   * @param miterLimit Miter limit of the stroke.
   */
  Boxd strokeMiterBounds(double strokeWidth, double miterLimit) const;

  /**
   * Get a point on the spline.
   *
   * @param index Index of the command in the spline.
   * @param t Position on the segment, between 0.0 and 1.0.
   * @return Vector2d Point at the specified position.
   */
  Vector2d pointAt(size_t index, double t) const;

  /**
   * Get the un-normalized tangent vector on the spline.
   *
   * @param index Index of the command in the spline.
   * @param t Position on the segment, between 0.0 and 1.0.
   * @return Vector2d Tangent vector at the specified position.
   */
  Vector2d tangentAt(size_t index, double t) const;

  /**
   * Get the normal vector on the spline.
   *
   * @param index Index of the command in the spline.
   * @param t Position on the segment, between 0.0 and 1.0.
   * @return Vector2d Normal vector at the specified position.
   */
  Vector2d normalAt(size_t index, double t) const;

  /**
   * Get the vertices of the path, including the orientation. Used to place markers for \ref
   * xml_marker.
   */
  std::vector<Vertex> vertices() const;

  /**
   * Returns true if this path contains the given point within its fill.
   *
   * @param point Point to check.
   * @param fillRule Fill rule to use, defaults to \ref FillRule::NonZero.
   */
  bool isInside(const Vector2d& point, FillRule fillRule = FillRule::NonZero) const;

  /**
   * Returns true if this path contains the given point within its stroke.
   *
   * @param point Point to check.
   * @param strokeWidth Width of the stroke.
   */
  bool isOnPath(const Vector2d& point, double strokeWidth) const;

  /**
   * Ostream output operator, outputs a human-readable representation of the spline.
   *
   * @param os Output stream.
   * @param spline Spline to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const PathSpline& spline);

private:
  /**
   * Get the starting point of a command.
   *
   * @param index Index of the command.
   * @return Vector2d Starting point of the command.
   */
  Vector2d startPoint(size_t index) const;

  /**
   * Get the ending point of a command.
   *
   * @param index Index of the command.
   * @return Vector2d Ending point of the command.
   */
  Vector2d endPoint(size_t index) const;

  /**
   * Auto-reopen the path if it is closed. This will reissue the last moveTo() command,
   * starting a new path at the same start coordinate.
   */
  void maybeAutoReopen();

  std::vector<Vector2d> points_;   //!< Vector of points in the spline.
  std::vector<Command> commands_;  //!< Vector of commands that define how the points are connected.

  /// Index of the last MoveTo point in \ref points_.
  size_t moveToPointIndex_ = size_t(-1);

  /// Index of the start of the current segment (if it is open), pointing to the MoveTo command.
  size_t currentSegmentStartCommandIndex_ = size_t(-1);

  /// True if the path is closed, but it may auto-reopen and MoveTo on the next draw command.
  /// This enables sequences such as "M 0 0 1 1 z L -1 -1" which close the path and then draw
  /// a new line.
  bool mayAutoReopen_ = false;
};

}  // namespace donner::svg
