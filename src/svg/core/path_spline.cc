#include "src/svg/core/path_spline.h"
#include "src/base/utils.h"

namespace donner {

Boxd PathSpline::Bounds() const {
  UTILS_RELEASE_ASSERT(!IsEmpty());

  Boxd box = Boxd::CreateEmpty(points_.front());
  Vector2d current;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];

    switch (command.type) {
      case CommandType::MoveTo:
      case CommandType::LineTo:
        current = points_[command.index];
        box.AddPoint(current);
        break;
      case CommandType::CurveTo: {
        const Vector2d& curve_start = current;
        const Vector2d& control_point_1 = points_[command.index];
        const Vector2d& control_point_2 = points_[command.index + 1];
        const Vector2d& curve_end = points_[command.index + 2];

        box.AddPoint(curve_start);  // Absolute start point.
        box.AddPoint(curve_end);    // Absolute end point.
        current = curve_end;

        // Find coefficients.
        // In the form of at^2 + bt + c, reduced from the derivative of:
        // (1 - t)^3 P_0 + 3(1 - t)^2 t P_1 + 3(1 - t) t^2 P_2 + t^3 P_3,
        //
        // Which is:
        // 3(P_1 - P_0)(1 - t)^2 + 6(P_2 - P_1) * t * (1 - t) + 3(P_3 - P_2)t^2
        //
        // References:
        // * http://www.cs.sunysb.edu/~qin/courses/geometry/4.pdf
        // * http://en.wikipedia.org/wiki/Bezier_curve#Examination_of_cases
        // * http://www.cs.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/Bezier/bezier-der.html
        Vector2d a_coeff =
            3.0 * (-curve_start + 3.0 * control_point_1 - 3.0 * control_point_2 + curve_end);
        Vector2d b_coeff = 6.0 * (curve_start + control_point_2 - 2.0 * control_point_1);
        Vector2d c_coeff = 3.0 * (-curve_start + control_point_1);

        // Add the x extrema.
        if (NearZero(a_coeff.x)) {
          if (!NearZero(b_coeff.x)) {
            const double t = -c_coeff.x / b_coeff.x;
            if (t >= 0.0 && t <= 1.0) {
              box.AddPoint(PointAt(i, t));
            }
          }
        } else {
          // Solve using the quadratic formula.
          const QuadraticSolution<double> res = SolveQuadratic(a_coeff.x, b_coeff.x, c_coeff.x);

          if (res.has_solution) {
            if (res.solution[0] >= 0.0 && res.solution[0] <= 1.0) {
              box.AddPoint(PointAt(i, res.solution[0]));
            }

            if (res.solution[1] >= 0.0 && res.solution[1] <= 1.0) {
              box.AddPoint(PointAt(i, res.solution[1]));
            }
          }
        }

        // Add the y extrema.
        if (NearZero(a_coeff.y)) {
          if (!NearZero(b_coeff.y)) {
            const double t = -c_coeff.y / b_coeff.y;
            if (t >= 0.0 && t <= 1.0) {
              box.AddPoint(PointAt(i, t));
            }
          }
        } else {
          // Solve using the quadratic formula.
          QuadraticSolution<double> res = SolveQuadratic(a_coeff.y, b_coeff.y, c_coeff.y);

          if (res.has_solution) {
            if (res.solution[0] >= 0.0 && res.solution[0] <= 1.0) {
              box.AddPoint(PointAt(i, res.solution[0]));
            }

            if (res.solution[1] >= 0.0 && res.solution[1] <= 1.0) {
              box.AddPoint(PointAt(i, res.solution[1]));
            }
          }
        }

        break;
      }
    }
  }

  return box;
}

// Internal helper function, adds the extrema created by the miter joint
// when provided the tangents of the joint, as well as the stroke properties.
//
// @param box Bounding box handle.
// @param current_point Line intersection point.
// @param tangent0 (Un-normalized) Tangent of first line.
// @param tangent1 (Un-normalized) Tangent of second line.
// @param stroke_width Width of stroke.
// @param miter_limit Miter limit of the stroke.
static void ComputeMiter(Boxd& box, const Vector2d& current_point, const Vector2d& tangent0,
                         const Vector2d& tangent1, double stroke_width, double miter_limit) {
  // Do not use dot product because the tangents aren't normalized.
  const double angle0 = tangent0.Angle();
  const double angle1 = tangent1.Angle();
  const double half_intersection_angle = Min(MathConstants<double>::kPi - Abs(angle0 - angle1),
                                             MathConstants<double>::kPi - Abs(angle1 - angle0)) *
                                         0.5;

  // epsilon is a magic number, to make it so our miter limit triggers at the same threshold as
  // cairo.
  const double kEpsilon = 0.03;

  double miter_len = stroke_width / tan(half_intersection_angle);
  if (miter_len / stroke_width < miter_limit - kEpsilon) {
    // We haven't exceeded the miter limit, compute the extrema.
    const double joint_angle =
        (angle0 + angle1 - MathConstants<double>::kPi) * 0.5;  // Direction of joint.
    box.AddPoint(current_point + miter_len * Vector2d(cos(joint_angle), sin(joint_angle)));
  }
}

Boxd PathSpline::StrokeMiterBounds(double stroke_width, double miter_limit) const {
  UTILS_RELEASE_ASSERT(!IsEmpty());

  Boxd box = Boxd::CreateEmpty(points_.front());
  Vector2d current;

  static constexpr size_t kNPos = ~size_t(0);
  size_t last_index = kNPos;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];

    switch (command.type) {
      case CommandType::LineTo: {
        if (last_index != kNPos) {
          Vector2d last_tangent = TangentAt(last_index, 1.0);
          Vector2d tangent = TangentAt(i, 0.0);

          ComputeMiter(box, current, last_tangent, tangent, stroke_width, miter_limit);
        }

        current = points_[command.index];
        last_index = i;
        break;
      }
      case CommandType::CurveTo: {
        if (last_index != kNPos) {
          Vector2d last_tangent = TangentAt(last_index, 1.0);
          Vector2d tangent = TangentAt(i, 0.0);

          ComputeMiter(box, current, last_tangent, tangent, stroke_width, miter_limit);
        }

        current = points_[command.index + 2];
        last_index = i;
        break;
      }

      default: last_index = kNPos; break;
    }
  }

  return box;
}

Vector2d PathSpline::PointAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0 && t <= 1.0 && "t out of range");

  const Command& command = commands_.at(index);

  switch (command.type) {
    case CommandType::MoveTo: return points_[command.index];
    case CommandType::LineTo: {
      // Determine the current point, which is the last defined point.
      assert(command.index > 0);
      const Vector2d& current_point = points_[command.index - 1];
      const double rev_t = 1.0 - t;

      return rev_t * current_point + t * points_[command.index];
    }
    case CommandType::CurveTo: {
      // Determine the current point, which is the last defined point.
      assert(command.index > 0);
      const Vector2d& current_point = points_[command.index - 1];
      const double rev_t = 1.0 - t;

      return rev_t * rev_t * rev_t * current_point               // (1 - t)^3 * p_0
             + 3.0 * t * rev_t * rev_t * points_[command.index]  // + 3t(1 - t)^2 * p_1
             + 3.0 * t * t * rev_t * points_[command.index + 1]  // + 3 t^2 (1 - t) * p_2
             + t * t * t * points_[command.index + 2];           // + t^3 * p_3
    }
    default: return Vector2d::Zero();
  }
}

Vector2d PathSpline::TangentAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0 && t <= 1.0 && "t out of range");

  const Command& command = commands_.at(index);

  switch (command.type) {
    case CommandType::MoveTo:
      if (index + 1 < commands_.size()) {
        return TangentAt(index + 1, 0.0);
      } else {
        return Vector2d::Zero();
      }
    case CommandType::LineTo: {
      assert(command.index > 0);
      return points_[command.index] - points_[command.index - 1];
    }
    case CommandType::CurveTo: {
      assert(command.index > 0);
      const double rev_t = 1.0 - t;

      // The tangent of a bezier curve is proportional to its first derivative. The derivative is:
      //
      // 3(P_1 - P_0)(1 - t)^2 + 6(P_2 - P_1) * t * (1 - t) + 3(P_3 - P_2)t^2
      //
      // Basically, the derivative of a cubic bezier curve is three times the
      // difference between two quadratic bezier curves.  See Bounds() for more details.
      const Vector2d p_1_0 = (points_[command.index] - points_[command.index - 1]);
      const Vector2d p_2_1 = (points_[command.index + 1] - points_[command.index]);
      const Vector2d p_3_2 = (points_[command.index + 2] - points_[command.index + 1]);

      return 3.0 * (rev_t * rev_t * p_1_0      // (1 - t)^2 * (P_1 - P_0)
                    + 2.0 * t * rev_t * p_2_1  // (1 - t) * t * (P_2 - P_1)
                    + t * t * p_3_2            // t^2 * (P_3 - P_2)
                    );
    }
    default: return Vector2d::Zero();
  }
}

Vector2d PathSpline::NormalAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0 && t <= 1.0 && "t out of range");

  const Command& command = commands_.at(index);

  switch (command.type) {
    case CommandType::MoveTo:
      if (index + 1 < commands_.size()) {
        return NormalAt(index + 1, 0.0);
      } else {
        return Vector2d::Zero();
      }

    case CommandType::LineTo: {
      assert(command.index > 0);
      const Vector2d tangent = (points_[command.index] - points_[command.index - 1]);
      return Vector2d(-tangent.y, tangent.x);
    }
    case CommandType::CurveTo: {
      assert(command.index > 0);
      const double rev_t = 1.0 - t;

      // The normal of a bezier curve is proportional to its second
      // derivative.  The second derivative is:
      //
      // 6 [ (1 - t) * (P_2 - 2 P_1 + P_0) + t * (P_3 - 2 P_2 + P_1) ]
      const Vector2d p_2_1_0 =
          (points_[command.index + 1] - 2.0 * points_[command.index] + points_[command.index - 1]);
      const Vector2d p_3_2_1 =
          (points_[command.index + 2] - 2.0 * points_[command.index + 1] + points_[command.index]);

      return 6.0 * (rev_t * p_2_1_0 + t * p_3_2_1);
    }

    default: return Vector2d::Zero();
  }
}

PathSpline::Builder::Builder() = default;

PathSpline::Builder& PathSpline::Builder::MoveTo(const Vector2d& point) {
  // As an optimization, if the last command was a MoveTo replace it with the new point.
  if (!commands_.empty() && commands_.back().type == CommandType::MoveTo) {
    points_[commands_.back().index] = point;
  } else {
    const size_t index = points_.size();

    points_.push_back(point);
    commands_.push_back({CommandType::MoveTo, index});

    moveto_index_ = index;
  }
  return *this;
}

PathSpline::Builder& PathSpline::Builder::LineTo(const Vector2d& point) {
  UTILS_RELEASE_ASSERT(moveto_index_ != kNPos && "LineTo without calling MoveTo first");

  const size_t index = points_.size();

  points_.push_back(point);
  commands_.push_back({CommandType::LineTo, index});
  return *this;
}

PathSpline::Builder& PathSpline::Builder::CurveTo(const Vector2d& point1, const Vector2d& point2,
                                                  const Vector2d& point3) {
  UTILS_RELEASE_ASSERT(moveto_index_ != kNPos && "CurveTo without calling MoveTo first");

  const size_t index = points_.size();
  points_.push_back(point1);
  points_.push_back(point2);
  points_.push_back(point3);
  commands_.push_back({CommandType::CurveTo, index});

  return *this;
}

// B.2.5. Correction of out-of-range radii
// https://www.w3.org/TR/SVG/implnote.html#ArcCorrectionOutOfRangeRadii
static Vector2d CorrectArcRadius(const Vector2d& radius, const Vector2d& major_axis) {
  // eq. 6.1
  const Vector2d abs_radius = Vector2d(Abs(radius.x), Abs(radius.y));

  // eq. 6.2
  const double lambda = (major_axis.x * major_axis.x) / (abs_radius.x * abs_radius.x) +
                        (major_axis.y * major_axis.y) / (abs_radius.y * abs_radius.y);

  // eq. 6.3
  if (lambda > 1.0) {
    return abs_radius * sqrt(lambda);
  } else {
    return abs_radius;
  }
}

// eq. 5.2 from https://www.w3.org/TR/SVG/implnote.html#ArcConversionEndpointToCenter
static Vector2d EllipseCenterForArc(const Vector2d& radius, const Vector2d& axis,
                                    bool large_arc_flag, bool sweep_flag) {
  double k = radius.x * radius.x * axis.y * axis.y + radius.y * radius.y * axis.x * axis.x;
  assert(!NearZero(k));

  k = sqrt(Abs((radius.x * radius.x * radius.y * radius.y) / k - 1.0));
  if (sweep_flag == large_arc_flag) {
    k = -k;
  }

  return Vector2d(k * radius.x * axis.y / radius.y, -k * radius.y * axis.x / radius.x);
}

PathSpline::Builder& PathSpline::Builder::ArcTo(const Vector2d& radius, double rotation_radians,
                                                bool large_arc_flag, bool sweep_flag,
                                                const Vector2d& end_point) {
  // See Appendix F.6 Elliptical arc implementation notes
  // http://www.w3.org/TR/SVG/implnote.html#ArcImplementationNotes

  // Determine the current point, which is the last point defined.
  const Vector2d current_point = points_.back();

  if (UTILS_PREDICT_FALSE(current_point == end_point)) {
    // No-op, the end point is the current position.
    return *this;
  }

  if (UTILS_PREDICT_FALSE(NearZero(radius.x) || NearZero(radius.y))) {
    // Zero radius falls back to a line segment.
    return LineTo(end_point);
  }

  // X-axis of the arc.
  const double sin_rotation = sin(rotation_radians);
  const double cos_rotation = cos(rotation_radians);

  // Rotate the extent to find the major axis.
  const Vector2d extent = (current_point - end_point) * 0.5;
  const Vector2d major_axis = extent.Rotate(cos_rotation, -sin_rotation);

  const Vector2d ellipse_radius = CorrectArcRadius(radius, major_axis);

  const Vector2d center_no_rotation =
      EllipseCenterForArc(ellipse_radius, major_axis, large_arc_flag, sweep_flag);
  const Vector2d center =
      center_no_rotation.Rotate(cos_rotation, sin_rotation) + (current_point + end_point) * 0.5;

  // Compute start angle.
  const Vector2d intersect_start = (major_axis - center_no_rotation) / ellipse_radius;
  const Vector2d intersect_end = (-major_axis - center_no_rotation) / ellipse_radius;

  double k = intersect_start.Length();
  if (NearZero(k)) {
    return *this;
  }

  k = Clamp(intersect_start.x / k, -1.0, 1.0);
  double theta = acos(k);
  if (intersect_start.y < 0.0) {
    theta = -theta;
  }

  // Compute delta_theta.
  k = sqrt(intersect_start.LengthSquared() * intersect_end.LengthSquared());
  if (NearZero(k)) {
    return *this;
  }

  k = Clamp(intersect_start.Dot(intersect_end) / k, -1.0, 1.0);

  double delta_theta = acos(k);
  if (intersect_start.x * intersect_end.y - intersect_end.x * intersect_start.y < 0.0) {
    delta_theta = -delta_theta;
  }

  if (sweep_flag && delta_theta < 0.0) {
    delta_theta += MathConstants<double>::kPi * 2.0;
  } else if (!sweep_flag && delta_theta > 0.0) {
    delta_theta -= MathConstants<double>::kPi * 2.0;
  }

  // Now draw the arc.
  size_t num_segs = (size_t)ceil(Abs(delta_theta / (MathConstants<double>::kPi * 0.5 + 0.001)));
  Vector2d dir(cos_rotation, sin_rotation);
  double theta_increment = delta_theta / double(num_segs);

  // Draw num_segs segments.
  for (size_t i = 0; i < num_segs; ++i) {
    // Determine the properties of the current segment.
    const double theta_start = theta + double(i) * theta_increment;
    const double theta_end = theta + double(i + 1) * theta_increment;

    const double theta_half = 0.5 * (theta_end - theta_start);

    const double sin_half_theta_half = sin(theta_half * 0.5);
    const double t = (8.0 / 3.0) * sin_half_theta_half * sin_half_theta_half / sin(theta_half);

    const double cos_theta_start = cos(theta_start);
    const double sin_theta_start = sin(theta_start);
    const Vector2d point1 = ellipse_radius * Vector2d(cos_theta_start - t * sin_theta_start,
                                                      sin_theta_start + t * cos_theta_start);

    const double cos_theta_end = cos(theta_end);
    const double sin_theta_end = sin(theta_end);
    const Vector2d point3 = ellipse_radius * Vector2d(cos_theta_end, sin_theta_end);

    const Vector2d point2 =
        point3 + ellipse_radius * Vector2d(t * sin_theta_end, -t * cos_theta_end);

    // Draw a curve for this segment.
    CurveTo(center + point1.Rotate(dir.x, dir.y), center + point2.Rotate(dir.x, dir.y),
            center + point3.Rotate(dir.x, dir.y));
  }

  return *this;
}

PathSpline::Builder& PathSpline::Builder::ClosePath() {
  UTILS_RELEASE_ASSERT((moveto_index_ != kNPos || !commands_.empty()) &&
                       "ClosePath without an open path");

  // Move back to the last MoveTo.
  commands_.push_back({CommandType::MoveTo, commands_[moveto_index_].index});
  moveto_index_ = commands_.size() - 1;

  return *this;
}

PathSpline::Builder& PathSpline::Builder::Ellipse(const Vector2d& center, const Vector2d& radius) {
  // Approximate an ellipse using four bezier curves.
  // 4/3 * (1 - cos(45 deg) / sin(45 deg) = 4/3 * (sqrt 2) - 1
  const double kArcMagic = 0.5522847498;

  // Start at theta = 0, or (radius.x, 0.0).
  MoveTo(center + Vector2d(radius.x, 0.0));

  // First curve, to (0.0, -radius.y).
  CurveTo(center + Vector2d(radius.x, -radius.y * kArcMagic),
          center + Vector2d(radius.x * kArcMagic, -radius.y), center + Vector2d(0.0, -radius.y));

  // Second curve, to (-radius.x, 0.0).
  CurveTo(center + Vector2d(-radius.x * kArcMagic, -radius.y),
          center + Vector2d(-radius.x, -radius.y * kArcMagic), center + Vector2d(-radius.x, 0.0));

  // Third curve, to (0.0, radius.y).
  CurveTo(center + Vector2d(-radius.x, radius.y * kArcMagic),
          center + Vector2d(-radius.x * kArcMagic, radius.y), center + Vector2d(0.0, radius.y));

  // Fourth curve, to (radius.x, 0.0).
  CurveTo(center + Vector2d(radius.x * kArcMagic, radius.y),
          center + Vector2d(radius.x, radius.y * kArcMagic), center + Vector2d(radius.x, 0.0));

  ClosePath();
  return *this;
}

PathSpline::Builder& PathSpline::Builder::Circle(const Vector2d& center, double radius) {
  Vector2d ellipse_radius(radius, radius);
  Ellipse(center, ellipse_radius);
  return *this;
}

PathSpline PathSpline::Builder::Build() {
  UTILS_RELEASE_ASSERT(valid_ && "Builder can only be used once");
  valid_ = false;
  return PathSpline(std::move(points_), std::move(commands_));
}

PathSpline::PathSpline(std::vector<Vector2d>&& points, std::vector<Command>&& commands)
    : points_(std::move(points)), commands_(std::move(commands)) {}

}  // namespace donner
