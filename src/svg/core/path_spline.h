#pragma once

#include <vector>

#include "src/base/box.h"
#include "src/base/vector2.h"

namespace donner {

class PathSpline {
public:
  enum class CommandType { MoveTo, CurveTo, LineTo, ClosePath };

  struct Command {
    CommandType type;
    size_t point_index;
  };

  class Builder {
  public:
    Builder();

    // Move the starting point of the spline to a new point.
    Builder& moveTo(const Vector2d& point);

    // Draw a line from the current point to a new point.
    Builder& lineTo(const Vector2d& point);

    // Draw a bezier curve from the current point to point3, using point1 and point2 as anchors.
    Builder& curveTo(const Vector2d& point1, const Vector2d& point2, const Vector2d& point3);

    // Add an elliptical arc to the path.
    //
    // @param radius Radius before rotation.
    // @param rotationRadians Rotation to the x-axis of the ellipse formed by the arc.
    // @param largeArcFlag false for arc length <= 180, true for arc >= 180.
    // @param sweep false for negative angle, true for positive angle.
    // @param endPoint End point.
    Builder& arcTo(const Vector2d& radius, double rotationRadians, bool largeArcFlag,
                   bool sweep_flag, const Vector2d& endPoint);

    // Close the path.
    //
    // An automatic straight line is drawn from the current point back to the initial point of
    // the current subpath.
    Builder& closePath();

    //
    // Complex drawing.
    //

    // Draw an ellipse.
    Builder& ellipse(const Vector2d& center, const Vector2d& radius);

    // Draw a circle.
    Builder& circle(const Vector2d& center, double radius);

    // Construct the PathSpline.
    PathSpline build();

  private:
    static constexpr size_t kNPos = ~size_t(0);

    bool valid_ = true;
    std::vector<Vector2d> points_;
    std::vector<Command> commands_;

    // Index of last moveto point in the points_ vector.
    size_t moveto_point_index_ = kNPos;
  };

  bool empty() const { return commands_.empty(); }

  const std::vector<Vector2d>& points() const { return points_; }
  const std::vector<Command>& commands() const { return commands_; }

  size_t size() const { return commands_.size(); }

  // Returns the bounding box for this spline.
  Boxd bounds() const;

  // Get the bounds of critical points created by miter joints when applying a stroke to this path.
  //
  // @param strokeWidth Width of stroke.
  // @param miterLimit Miter limit of the stroke.
  Boxd strokeMiterBounds(double strokeWidth, double miterLimit) const;

  // Get a point on the spline.
  //
  // @param idx Spline index.
  // @param t Position on spline, between 0.0 and 1.0.
  Vector2d pointAt(size_t index, double t) const;

  // Get the tangent vector on the spline.
  //
  // @param idx Spline index.
  // @param t Position on spline, between 0.0 and 1.0.
  Vector2d tangentAt(size_t index, double t) const;

  // Get the normal vector on the spline.
  //
  // @param idx Spline index.
  // @param t Position on spline, between 0.0 and 1.0.
  Vector2d normalAt(size_t index, double t) const;

private:
  PathSpline(std::vector<Vector2d>&& points, std::vector<Command>&& commands);

  // Get the start point of a command.
  //
  // @param idx Spline index.
  Vector2d startPoint(size_t index) const;

  std::vector<Vector2d> points_;
  std::vector<Command> commands_;
};

}  // namespace donner
