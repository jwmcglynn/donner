#pragma once
/// @file

#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/svg/core/FillRule.h"

namespace donner::svg {

/**
 * Container for a spline, which is a series of points connected by lines and curves.
 *
 * This is used to represent the `d` attribute of the \ref SVGPathElement (\ref xml_path), see
 * https://www.w3.org/TR/SVG2/paths.html#PathData. To parse SVG path data into a PathSpline, use the
 * \ref PathParser.
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
   * Note that these do not map 1:1 to the SVG path commands, since SVG path commands can be
   * simplified into these basic commands.
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
     * Draw a cubic bézier curve from the current point to a new point.
     *
     * Consumes 3 points:
     * - 1: First control point.
     * - 2: Second control point.
     * - 3: End point of the curve.
     */
    CurveTo,

    /**
     * Draw a line from the current point to a new point.
     *
     * Consumes 1 point:
     * - 1: End point of the line.
     */
    LineTo,

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
    size_t pointIndex;  ///< Index of the first point used by this command. The number of points
                        ///< consumed by the command is determined by the command type.

    /**
     * Construct a new Command.
     *
     * @param type Type of command.
     * @param pointIndex Index of the first point used by this command.
     * @see CommandType
     */
    Command(CommandType type, size_t pointIndex) : type(type), pointIndex(pointIndex) {}

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
   * Builder to construct a new PathSpline.
   *
   * ```
   * const PathSpline spline = PathSpline::Builder()
   *   .moveTo({0, 0})
   *   .lineTo({1, 0})
   *   .closePath()
   *   .build();
   * ```
   */
  class Builder {
  public:
    /**
     * Construct a new Builder.
     */
    Builder();

    /**
     * Move the starting point of the spline to a new point, creating a new subpath. If this is
     * called multiple times in a row, subsequent calls will replace the previous.
     *
     * @param point Point to move to.
     */
    Builder& moveTo(const Vector2d& point);

    /**
     * Draw a line from the current point to a new point.
     *
     * @param point End point of the line.
     */
    Builder& lineTo(const Vector2d& point);

    /**
     * Draw a bézier curve from the current point to \p point3, using \p point1 and \p point2 as
     * anchors.
     *
     * @param point1 First control point.
     * @param point2 Second control point.
     * @param point3 End point of the curve.
     */
    Builder& curveTo(const Vector2d& point1, const Vector2d& point2, const Vector2d& point3);

    /**
     * Add an elliptical arc to the path.
     *
     * @param radius Radius before rotation.
     * @param rotationRadians Rotation to the x-axis of the ellipse formed by the arc.
     * @param largeArcFlag false for arc length <= 180, true for arc >= 180.
     * @param sweepFlag false for negative angle, true for positive angle.
     * @param endPoint End point.
     */
    Builder& arcTo(const Vector2d& radius, double rotationRadians, bool largeArcFlag,
                   bool sweepFlag, const Vector2d& endPoint);

    /**
     * Close the path.
     *
     * An automatic straight line is drawn from the current point back to the initial point of
     * the current subpath.
     */
    Builder& closePath();

    //
    // Complex drawing.
    //

    /**
     * Draw an ellipse.
     *
     * @param center Center of the ellipse.
     * @param radius Ellipse radius, for both the x and y axis.
     */
    Builder& ellipse(const Vector2d& center, const Vector2d& radius);

    /**
     * Draw a circle
     *
     * @param center Center of the circle.
     * @param radius Radius.
     */
    Builder& circle(const Vector2d& center, double radius);

    /**
     * Construct the PathSpline.
     */
    PathSpline build();

  private:
    static constexpr size_t kNPos = ~size_t(0);

    bool valid_ = true;
    std::vector<Vector2d> points_;
    std::vector<Command> commands_;

    // Index of last moveto point in the points_ vector.
    size_t moveToPointIndex_ = kNPos;
  };

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
   * Returns the bounding box for this spline.
   */
  Boxd bounds() const;

  /**
   * Get the bounds of critical points created by miter joints when applying a stroke to this path.
   *
   * @param strokeWidth Width of stroke.
   * @param miterLimit Miter limit of the stroke.
   */
  Boxd strokeMiterBounds(double strokeWidth, double miterLimit) const;

  /**
   * Get a point on the spline.
   *
   * @param index Spline index.
   * @param t Position on spline, between 0.0 and 1.0.
   * @return Vector2d
   */
  Vector2d pointAt(size_t index, double t) const;

  /**
   * Get the tangent vector on the spline.
   *
   * @param index Spline index.
   * @param t Position on spline, between 0.0 and 1.0.
   */
  Vector2d tangentAt(size_t index, double t) const;

  /**
   * Get the normal vector on the spline.
   *
   * @param index Spline index.
   * @param t Position on spline, between 0.0 and 1.0.
   */
  Vector2d normalAt(size_t index, double t) const;

  /**
   * Returns true if this path contains the given point within its fill.
   *
   * @param point Point to check.
   * @param fillRule Fill rule to use, defaults to \ref FillRule::NonZero.
   */
  bool isInside(const Vector2d& point, FillRule fillRule = FillRule::NonZero) const;

  /**
   * Returns true if this path contains the given point within its stroke.
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
  PathSpline(std::vector<Vector2d>&& points,
             std::vector<Command>&& commands);  // NOLINT: Internal constructor.

  /**
   * Get the start point of a command.
   *
   * @param index Spline index.
   */
  Vector2d startPoint(size_t index) const;

  std::vector<Vector2d> points_;
  std::vector<Command> commands_;
};

}  // namespace donner::svg
