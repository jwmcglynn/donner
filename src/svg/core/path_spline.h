#pragma once

#include "src/base/box.h"
#include "src/base/vector2.h"

#include <vector>

namespace donner {

class PathSpline {
public:
  enum class CommandType { MoveTo, CurveTo, LineTo };

  struct Command {
    CommandType type;
    size_t index;
  };

  class Builder {
  public:
    Builder();

    // Move the starting point of the spline to a new point.
    Builder& MoveTo(const Vector2d& point);

    // Draw a line from the current point to a new point.
    Builder& LineTo(const Vector2d& point);

    // Draw a bezier curve from the current point to point3, using point1 and point2 as anchors.
    Builder& CurveTo(const Vector2d& point1, const Vector2d& point2, const Vector2d& point3);

    // Add an elliptical arc to the path.
    //
    // @param radius Radius before rotation.
    // @param rotation_radians Rotation to the x-axis of the ellipse formed by the arc.
    // @param large_arc_flag false for arc length <= 180, true for arc >= 180.
    // @param sweep false for negative angle, true for positive angle.
    // @param end_point End point.
    Builder& ArcTo(const Vector2d& radius, double rotation_radians, bool large_arc_flag,
                   bool sweep_flag, const Vector2d& end_point);

    // Close the path.
    Builder& ClosePath();

    //
    // Complex drawing.
    //

    // Draw an ellipse.
    Builder& Ellipse(const Vector2d& center, const Vector2d& radius);

    // Draw a circle.
    Builder& Circle(const Vector2d& center, double radius);

    // Construct the PathSpline.
    PathSpline Build();

  private:
    static constexpr size_t kNPos = ~size_t(0);

    std::vector<Vector2d> points_;
    std::vector<Command> commands_;

    // Index of last moveto command in the commands_ vector.
    size_t moveto_index_ = kNPos;
  };

  bool IsEmpty() const { return commands_.empty(); }

  const std::vector<Vector2d>& Points() const { return points_; }
  const std::vector<Command>& Commands() const { return commands_; }

  size_t Size() const { return commands_.size(); }

  // Returns the bounding box for this spline.
  Boxd Bounds() const;

  // Get the bounds of critical points created by miter joints when applying a stroke to this path.
  //
  // @param stroke_width Width of stroke.
  // @param miter_limit Miter limit of the stroke.
  Boxd StrokeMiterBounds(double stroke_width, double miter_limit) const;

  // Get a point on the spline.
  //
  // @param idx Spline index.
  // @param t Position on spline, between 0.0 and 1.0.
  Vector2d PointAt(size_t index, double t) const;

  // Get the tangent vector on the spline.
  //
  // @param idx Spline index.
  // @param t Position on spline, between 0.0 and 1.0.
  Vector2d TangentAt(size_t index, double t) const;

  // Get the normal vector on the spline.
  //
  // @param idx Spline index.
  // @param t Position on spline, between 0.0 and 1.0.
  Vector2d NormalAt(size_t index, double t) const;

private:
  PathSpline(std::vector<Vector2d>&& points, std::vector<Command>&& commands);

  std::vector<Vector2d> points_;
  std::vector<Command> commands_;
};

}  // namespace donner
