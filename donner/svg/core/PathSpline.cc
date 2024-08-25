#include "donner/svg/core/PathSpline.h"

#include "donner/base/MathUtils.h"
#include "donner/base/Utils.h"

namespace donner::svg {

namespace {

/**
 * Subdivide a cubic curve into two quadratics, and measure the length of the resulting curve.
 * Subdivide until the given accuracy \ref tolerance is reached.
 *
 * @param points The points of the curve segment.
 * @param tolerance Controls the accuracy of the subdivision, based a which limits
 * the amount of error in the length.
 * @param depth Current depth of the subdivision.
 */
double SubdivideAndMeasureCubic(const std::array<Vector2d, 4>& points, double tolerance,
                                int depth = 0) {
  const int maxDepth = 20;

  auto midPoint = [](const Vector2d& p1, const Vector2d& p2) { return (p1 + p2) * 0.5; };

  const Vector2d& p0 = points[0];
  const Vector2d& p1 = points[1];
  const Vector2d& p2 = points[2];
  const Vector2d& p3 = points[3];

  const Vector2d p01 = midPoint(p0, p1);
  const Vector2d p12 = midPoint(p1, p2);
  const Vector2d p23 = midPoint(p2, p3);
  const Vector2d p012 = midPoint(p01, p12);
  const Vector2d p123 = midPoint(p12, p23);
  const Vector2d p0123 = midPoint(p012, p123);

  const double chord = p0.distance(p3);
  const double contNet = p0.distance(p1) + p1.distance(p2) + p2.distance(p3);

  if ((contNet - chord) <= tolerance || depth >= maxDepth) {
    return (chord + contNet) / 2;
  }

  const std::array<Vector2d, 4> left = {p0, p01, p012, p0123};
  const std::array<Vector2d, 4> right = {p0123, p123, p23, p3};

  return SubdivideAndMeasureCubic(left, tolerance, depth + 1) +
         SubdivideAndMeasureCubic(right, tolerance, depth + 1);
};

/**
 * Internal helper function, adds the extrema created by the miter joint
 * when provided the tangents of the joint, as well as the stroke properties.
 *
 * @param box Bounding box handle.
 * @param currentPoint Line intersection point.
 * @param tangent0 (Un-normalized) Tangent of first line.
 * @param tangent1 (Un-normalized) Tangent of second line.
 * @param strokeWidth Width of stroke.
 * @param miterLimit Miter limit of the stroke.
 */
void ComputeMiter(Boxd& box, const Vector2d& currentPoint, const Vector2d& tangent0,
                  const Vector2d& tangent1, double strokeWidth, double miterLimit) {
  const double intersectionAngle = tangent0.angleWith(-tangent1);

  // If we're under the miter limit, the miter applies. However, don't apply it if the tangents are
  // colinear, since it would not apply in a consistent direction.
  const double miterLength = strokeWidth / std::sin(intersectionAngle * 0.5);
  if (miterLength < miterLimit && !NearEquals(intersectionAngle, MathConstants<double>::kPi)) {
    // We haven't exceeded the miter limit, compute the extrema.
    const double jointAngle = (tangent0 - tangent1).angle();
    box.addPoint(currentPoint + miterLength * Vector2d(std::cos(jointAngle), std::sin(jointAngle)));
  }
}

int WindingNumberContribution(const Vector2d& p0, const Vector2d& p1, const Vector2d& point) {
  // Check if the segment crosses the horizontal ray to the right of the point
  if (p0.y <= point.y) {
    if (p1.y > point.y) {  // Upward crossing
      // Compute the determinant (cross product)
      double det = (p1 - p0).cross(point - p0);
      if (det > 0) {
        return 1;  // Winding number increases
      }
    }
  } else {
    if (p1.y <= point.y) {  // Downward crossing
      double det = (p1 - p0).cross(point - p0);
      if (det < 0) {
        return -1;  // Winding number decreases
      }
    }
  }
  return 0;  // No contribution
}

double DistanceFromPointToLine(const Vector2d& p, const Vector2d& a, const Vector2d& b) {
  const Vector2d ab = b - a;
  const Vector2d ap = p - a;
  const double abLengthSquared = ab.lengthSquared();

  if (NearZero(abLengthSquared)) {
    // 'a' and 'b' are the same point
    return (p - a).length();
  }

  double t = ap.dot(ab) / abLengthSquared;
  t = Clamp(t, 0.0, 1.0);

  const Vector2d projection = a + t * ab;
  return (p - projection).length();
}

bool IsCurveFlatEnough(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                       const Vector2d& p3, double tolerance) {
  // Use the "maximum distance from control points to the line p0-p3" as a flatness metric
  const double distance1 = DistanceFromPointToLine(p1, p0, p3);
  const double distance2 = DistanceFromPointToLine(p2, p0, p3);
  return (distance1 <= tolerance) && (distance2 <= tolerance);
}

int WindingNumberContributionCurve(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                   const Vector2d& p3, const Vector2d& point, double tolerance,
                                   int depth = 0) {
  // Define maximum recursion depth to prevent infinite recursion
  const int maxDepth = 10;

  // Check if the curve is flat enough
  if (IsCurveFlatEnough(p0, p1, p2, p3, tolerance) || depth >= maxDepth) {
    // Approximate the curve with a straight line segment
    return WindingNumberContribution(p0, p3, point);
  }

  // Subdivide the curve using De Casteljau's algorithm
  const Vector2d p01 = (p0 + p1) * 0.5;
  const Vector2d p12 = (p1 + p2) * 0.5;
  const Vector2d p23 = (p2 + p3) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  // Recursively compute winding number contributions for both subdivided curves
  int winding = 0;
  winding += WindingNumberContributionCurve(p0, p01, p012, p0123, point, tolerance, depth + 1);
  winding += WindingNumberContributionCurve(p0123, p123, p23, p3, point, tolerance, depth + 1);
  return winding;
}

bool IsPointOnCubicBezier(const Vector2d& point, const Vector2d& p0, const Vector2d& p1,
                          const Vector2d& p2, const Vector2d& p3, double tolerance, int depth = 0) {
  // Define maximum recursion depth to prevent infinite recursion
  const int maxDepth = 10;

  // Use the subdivision approach to check if the point is close to any segment of the curve
  if (IsCurveFlatEnough(p0, p1, p2, p3, tolerance) || depth > maxDepth) {
    // Approximate the curve with a straight line segment and check the distance
    return DistanceFromPointToLine(point, p0, p3) <= tolerance;
  }

  // Subdivide the curve and check each segment
  const Vector2d p01 = (p0 + p1) * 0.5;
  const Vector2d p12 = (p1 + p2) * 0.5;
  const Vector2d p23 = (p2 + p3) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  return IsPointOnCubicBezier(point, p0, p01, p012, p0123, tolerance, depth + 1) ||
         IsPointOnCubicBezier(point, p0123, p123, p23, p3, tolerance, depth + 1);
}

}  // namespace

std::ostream& operator<<(std::ostream& os, PathSpline::CommandType type) {
  switch (type) {
    case PathSpline::CommandType::MoveTo: os << "CommandType::MoveTo"; break;
    case PathSpline::CommandType::LineTo: os << "CommandType::LineTo"; break;
    case PathSpline::CommandType::CurveTo: os << "CommandType::CurveTo"; break;
    case PathSpline::CommandType::ClosePath: os << "CommandType::ClosePath"; break;
    default: UTILS_RELEASE_ASSERT(false && "Invalid command");
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const PathSpline::Command& command) {
  return os << "Command {" << command.type << ", " << command.pointIndex << "}";
}

std::ostream& operator<<(std::ostream& os, const PathSpline& spline) {
  os << "PathSpline {\n";
  os << " points: [\n  ";
  for (const auto& pt : spline.points()) {
    os << pt << ", ";
  }
  os << "\n ]\n commands: [\n  ";
  for (const auto& command : spline.commands()) {
    os << command << ", ";
  }
  return os << "\n ]\n}\n";
}

double PathSpline::pathLength() const {
  const double kTolerance = 0.001;

  double totalLength = 0.0;
  Vector2d startPoint;

  for (const Command& command : commands_) {
    switch (command.type) {
      case CommandType::MoveTo: startPoint = points_[command.pointIndex]; break;
      case CommandType::LineTo: {
        const Vector2d& endPoint = points_[command.pointIndex];
        totalLength += startPoint.distance(endPoint);
        startPoint = endPoint;
        break;
      }
      case CommandType::CurveTo: {
        const std::array<Vector2d, 4> bezierPoints = {startPoint, points_[command.pointIndex],
                                                      points_[command.pointIndex + 1],
                                                      points_[command.pointIndex + 2]};
        totalLength += SubdivideAndMeasureCubic(bezierPoints, kTolerance);
        startPoint = points_[command.pointIndex + 2];
        break;
      }
      case CommandType::ClosePath:
        // Optional: handle ClosePath if necessary
        break;
    }
  }

  return totalLength;
}

Boxd PathSpline::bounds() const {
  UTILS_RELEASE_ASSERT(!empty());

  Boxd box = Boxd::CreateEmpty(points_.front());
  Vector2d current;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];

    switch (command.type) {
      case CommandType::MoveTo:
      case CommandType::LineTo:
      case CommandType::ClosePath:
        current = points_[command.pointIndex];
        box.addPoint(current);
        break;
      case CommandType::CurveTo: {
        const Vector2d& curveStart = current;
        const Vector2d& controlPoint1 = points_[command.pointIndex];
        const Vector2d& controlPoint2 = points_[command.pointIndex + 1];
        const Vector2d& curveEnd = points_[command.pointIndex + 2];

        box.addPoint(curveStart);  // Absolute start point.
        box.addPoint(curveEnd);    // Absolute end point.
        current = curveEnd;

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
        const Vector2d a =
            3.0 * (-curveStart + 3.0 * controlPoint1 - 3.0 * controlPoint2 + curveEnd);
        const Vector2d b = 6.0 * (curveStart + controlPoint2 - 2.0 * controlPoint1);
        const Vector2d c = 3.0 * (-curveStart + controlPoint1);

        // Add the x extrema.
        if (NearZero(a.x)) {
          if (!NearZero(b.x)) {
            const double t = -c.x / b.x;
            if (t >= 0.0 && t <= 1.0) {
              box.addPoint(pointAt(i, t));
            }
          }
        } else {
          // Solve using the quadratic formula.
          const QuadraticSolution<double> res = SolveQuadratic(a.x, b.x, c.x);

          if (res.hasSolution) {
            if (res.solution[0] >= 0.0 && res.solution[0] <= 1.0) {
              box.addPoint(pointAt(i, res.solution[0]));
            }

            if (res.solution[1] >= 0.0 && res.solution[1] <= 1.0) {
              box.addPoint(pointAt(i, res.solution[1]));
            }
          }
        }

        // Add the y extrema.
        if (NearZero(a.y)) {
          if (!NearZero(b.y)) {
            const double t = -c.y / b.y;
            if (t >= 0.0 && t <= 1.0) {
              box.addPoint(pointAt(i, t));
            }
          }
        } else {
          // Solve using the quadratic formula.
          QuadraticSolution<double> res = SolveQuadratic(a.y, b.y, c.y);

          if (res.hasSolution) {
            if (res.solution[0] >= 0.0 && res.solution[0] <= 1.0) {
              box.addPoint(pointAt(i, res.solution[0]));
            }

            if (res.solution[1] >= 0.0 && res.solution[1] <= 1.0) {
              box.addPoint(pointAt(i, res.solution[1]));
            }
          }
        }

        break;
      }
    }
  }

  return box;
}

Boxd PathSpline::strokeMiterBounds(double strokeWidth, double miterLimit) const {
  UTILS_RELEASE_ASSERT(!empty());
  assert(strokeWidth > 0.0);
  assert(miterLimit >= 0.0);

  Boxd box = Boxd::CreateEmpty(points_.front());
  Vector2d current;

  static constexpr size_t kNPos = ~size_t(0);
  size_t lastIndex = kNPos;
  size_t lastMoveToIndex = kNPos;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];

    switch (command.type) {
      case CommandType::MoveTo: {
        current = points_[command.pointIndex];
        box.addPoint(current);

        lastIndex = kNPos;
        lastMoveToIndex = i;
        break;
      }
      case CommandType::ClosePath: {
        if (lastIndex != kNPos) {
          // For ClosePath, start with a standard line segment.
          const Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          const Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
          current = points_[command.pointIndex];

          // Then "join" it to the first segment of the subpath.
          const Vector2d joinTangent = tangentAt(lastMoveToIndex, 0.0);
          ComputeMiter(box, current, tangent, joinTangent, strokeWidth, miterLimit);
        }

        lastIndex = kNPos;
        break;
      }
      case CommandType::LineTo: {
        if (lastIndex != kNPos) {
          Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
        }

        current = points_[command.pointIndex];
        box.addPoint(current);
        lastIndex = i;
        break;
      }
      case CommandType::CurveTo: {
        if (lastIndex != kNPos) {
          Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
        }

        current = points_[command.pointIndex + 2];
        box.addPoint(current);
        lastIndex = i;
        break;
      }

      default: lastIndex = kNPos; break;
    }
  }

  return box;
}

Vector2d PathSpline::pointAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0.0 && t <= 1.0 && "t out of range");

  const Command& command = commands_.at(index);

  switch (command.type) {
    case CommandType::MoveTo: return points_[command.pointIndex];
    case CommandType::LineTo:
    case CommandType::ClosePath: {
      const Vector2d start = startPoint(index);
      const double rev_t = 1.0 - t;

      return rev_t * start + t * points_[command.pointIndex];
    }
    case CommandType::CurveTo: {
      // Determine the current point, which is the last defined point.
      const Vector2d start = startPoint(index);
      const double rev_t = 1.0 - t;

      return rev_t * rev_t * rev_t * start                            // (1 - t)^3 * p_0
             + 3.0 * t * rev_t * rev_t * points_[command.pointIndex]  // + 3t(1 - t)^2 * p_1
             + 3.0 * t * t * rev_t * points_[command.pointIndex + 1]  // + 3 t^2 (1 - t) * p_2
             + t * t * t * points_[command.pointIndex + 2];           // + t^3 * p_3
    }
    default: UTILS_RELEASE_ASSERT(false && "Unhandled command");
  }
}

Vector2d PathSpline::tangentAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0.0 && t <= 1.0 && "t out of range");

  const Command& command = commands_.at(index);

  switch (command.type) {
    case CommandType::MoveTo:
      if (index + 1 < commands_.size()) {
        return tangentAt(index + 1, 0.0);
      } else {
        return Vector2d::Zero();
      }
    case CommandType::LineTo:
    case CommandType::ClosePath: {
      return points_[command.pointIndex] - startPoint(index);
    }
    case CommandType::CurveTo: {
      assert(command.pointIndex > 0);
      const double rev_t = 1.0 - t;

      // The tangent of a bezier curve is proportional to its first derivative. The derivative is:
      //
      // 3(P_1 - P_0)(1 - t)^2 + 6(P_2 - P_1) * t * (1 - t) + 3(P_3 - P_2)t^2
      //
      // Basically, the derivative of a cubic bezier curve is three times the
      // difference between two quadratic bezier curves.  See Bounds() for more details.
      const Vector2d p_1_0 = points_[command.pointIndex] - startPoint(index);
      const Vector2d p_2_1 = points_[command.pointIndex + 1] - points_[command.pointIndex];
      const Vector2d p_3_2 = points_[command.pointIndex + 2] - points_[command.pointIndex + 1];

      return 3.0 * (rev_t * rev_t * p_1_0      // (1 - t)^2 * (P_1 - P_0)
                    + 2.0 * t * rev_t * p_2_1  // (1 - t) * t * (P_2 - P_1)
                    + t * t * p_3_2            // t^2 * (P_3 - P_2)
                   );
    }
    default: UTILS_RELEASE_ASSERT(false && "Unhandled command");
  }
}

Vector2d PathSpline::normalAt(size_t index, double t) const {
  const Vector2d tangent = tangentAt(index, t);
  return Vector2d(-tangent.y, tangent.x);
}

bool PathSpline::isInside(const Vector2d& point, FillRule fillRule) const {
  const double kTolerance = 0.1;

  int windingNumber = 0;
  Vector2d currentPoint;

  for (const Command& command : commands_) {
    switch (command.type) {
      case CommandType::MoveTo: currentPoint = points_[command.pointIndex]; break;

      case CommandType::ClosePath: [[fallthrough]];
      case CommandType::LineTo: {
        const Vector2d& endPoint = points_[command.pointIndex];
        if (DistanceFromPointToLine(point, currentPoint, endPoint) <= kTolerance) {
          return true;  // Point is on the line
        }
        windingNumber += WindingNumberContribution(currentPoint, endPoint, point);
        currentPoint = endPoint;
        break;
      }

      case CommandType::CurveTo: {
        const Vector2d& controlPoint1 = points_[command.pointIndex];
        const Vector2d& controlPoint2 = points_[command.pointIndex + 1];
        const Vector2d& endPoint = points_[command.pointIndex + 2];
        if (IsPointOnCubicBezier(point, currentPoint, controlPoint1, controlPoint2, endPoint,
                                 kTolerance)) {
          return true;  // Point is on the curve
        }
        windingNumber += WindingNumberContributionCurve(currentPoint, controlPoint1, controlPoint2,
                                                        endPoint, point, kTolerance);
        currentPoint = endPoint;
        break;
      }
    }
  }

  if (fillRule == FillRule::NonZero) {
    // Non-zero rule: The point is inside if the winding number is non-zero.
    return windingNumber != 0;
  } else if (fillRule == FillRule::EvenOdd) {
    // Even-odd rule: The point is inside if the winding number is odd.
    return (windingNumber % 2) != 0;
  }

  UTILS_UNREACHABLE();
}

bool PathSpline::isOnPath(const Vector2d& point, double strokeWidth) const {
  Vector2d currentPoint;

  for (const Command& command : commands_) {
    switch (command.type) {
      case CommandType::MoveTo: currentPoint = points_[command.pointIndex]; break;

      case CommandType::ClosePath: [[fallthrough]];
      case CommandType::LineTo: {
        const Vector2d& endPoint = points_[command.pointIndex];
        if (DistanceFromPointToLine(point, currentPoint, endPoint) <= strokeWidth) {
          return true;  // Point is on the line
        }
        currentPoint = endPoint;
        break;
      }

      case CommandType::CurveTo: {
        const Vector2d& controlPoint1 = points_[command.pointIndex];
        const Vector2d& controlPoint2 = points_[command.pointIndex + 1];
        const Vector2d& endPoint = points_[command.pointIndex + 2];
        if (IsPointOnCubicBezier(point, currentPoint, controlPoint1, controlPoint2, endPoint,
                                 strokeWidth)) {
          return true;  // Point is on the curve
        }
        currentPoint = endPoint;
        break;
      }
    }
  }

  return false;
}

Vector2d PathSpline::startPoint(size_t index) const {
  assert(index < commands_.size() && "index out of range");

  const Command& currentCommand = commands_[index];
  if (currentCommand.type == CommandType::MoveTo) {
    return points_[currentCommand.pointIndex];
  } else {
    assert(index > 0);  // First index should be a MoveTo, so this should not hit.
    const Command& prevCommand = commands_[index - 1];

    switch (prevCommand.type) {
      case CommandType::MoveTo:
      case CommandType::LineTo:
      case CommandType::ClosePath: {
        return points_[prevCommand.pointIndex];
      }
      case CommandType::CurveTo: {
        return points_[prevCommand.pointIndex + 2];
      }
      default: UTILS_RELEASE_ASSERT(false && "Unhandled command");
    }
  }
}

PathSpline::Builder::Builder() = default;

PathSpline::Builder& PathSpline::Builder::moveTo(const Vector2d& point) {
  // As an optimization, if the last command was a MoveTo replace it with the new point.
  if (!commands_.empty() && commands_.back().type == CommandType::MoveTo) {
    Command& lastCommand = commands_.back();

    // If this is MoveTo has a unique point, overwrite it, otherwise insert a new point.
    if (lastCommand.pointIndex + 1 == points_.size()) {
      points_[lastCommand.pointIndex] = point;
    } else {
      const size_t pointIndex = points_.size();
      points_.push_back(point);
      lastCommand.pointIndex = pointIndex;

      moveToPointIndex_ = pointIndex;
    }
  } else {
    const size_t pointIndex = points_.size();
    points_.push_back(point);
    commands_.emplace_back(CommandType::MoveTo, pointIndex);

    moveToPointIndex_ = pointIndex;
  }
  return *this;
}

PathSpline::Builder& PathSpline::Builder::lineTo(const Vector2d& point) {
  UTILS_RELEASE_ASSERT(moveToPointIndex_ != kNPos && "LineTo without calling MoveTo first");

  const size_t index = points_.size();

  points_.push_back(point);
  commands_.push_back({CommandType::LineTo, index});
  return *this;
}

PathSpline::Builder& PathSpline::Builder::curveTo(const Vector2d& point1, const Vector2d& point2,
                                                  const Vector2d& point3) {
  UTILS_RELEASE_ASSERT(moveToPointIndex_ != kNPos && "CurveTo without calling MoveTo first");

  const size_t index = points_.size();
  points_.push_back(point1);
  points_.push_back(point2);
  points_.push_back(point3);
  commands_.push_back({CommandType::CurveTo, index});

  return *this;
}

// B.2.5. Correction of out-of-range radii
// https://www.w3.org/TR/SVG/implnote.html#ArcCorrectionOutOfRangeRadii
static Vector2d CorrectArcRadius(const Vector2d& radius, const Vector2d& majorAxis) {
  // eq. 6.1
  const Vector2d absRadius = Vector2d(Abs(radius.x), Abs(radius.y));

  // eq. 6.2
  const double lambda = (majorAxis.x * majorAxis.x) / (absRadius.x * absRadius.x) +
                        (majorAxis.y * majorAxis.y) / (absRadius.y * absRadius.y);

  // eq. 6.3
  if (lambda > 1.0) {
    return absRadius * sqrt(lambda);
  } else {
    return absRadius;
  }
}

// eq. 5.2 from https://www.w3.org/TR/SVG/implnote.html#ArcConversionEndpointToCenter
static Vector2d EllipseCenterForArc(const Vector2d& radius, const Vector2d& axis, bool largeArcFlag,
                                    bool sweepFlag) {
  double k = radius.x * radius.x * axis.y * axis.y + radius.y * radius.y * axis.x * axis.x;
  assert(!NearZero(k));

  k = sqrt(Abs((radius.x * radius.x * radius.y * radius.y) / k - 1.0));
  if (sweepFlag == largeArcFlag) {
    k = -k;
  }

  return Vector2d(k * radius.x * axis.y / radius.y, -k * radius.y * axis.x / radius.x);
}

PathSpline::Builder& PathSpline::Builder::arcTo(const Vector2d& radius, double rotationRadians,
                                                bool largeArcFlag, bool sweepFlag,
                                                const Vector2d& endPoint) {
  // See Appendix F.6 Elliptical arc implementation notes
  // http://www.w3.org/TR/SVG/implnote.html#ArcImplementationNotes
  const double kDistanceSqEpsilon = 1e-14;  // Chosen unscientifically to avoid a NearZero assert
                                            // in EllipseCenterForArc, should be sufficiently
                                            // large so that x^4 > DBL_EPSILON.

  // Determine the current point, which is the last point defined.
  const Vector2d currentPoint = points_.back();

  if (UTILS_PREDICT_FALSE(NearZero(currentPoint.distanceSquared(endPoint), kDistanceSqEpsilon))) {
    // No-op, the end point is the current position.
    return *this;
  }

  if (UTILS_PREDICT_FALSE(NearZero(radius.x) || NearZero(radius.y))) {
    // Zero radius falls back to a line segment.
    return lineTo(endPoint);
  }

  // X-axis of the arc.
  const double sinRotation = std::sin(rotationRadians);
  const double cosRotation = std::cos(rotationRadians);

  // Rotate the extent to find the major axis.
  const Vector2d extent = (currentPoint - endPoint) * 0.5;
  const Vector2d majorAxis = extent.rotate(cosRotation, -sinRotation);

  const Vector2d ellipseRadius = CorrectArcRadius(radius, majorAxis);

  const Vector2d centerNoRotation =
      EllipseCenterForArc(ellipseRadius, majorAxis, largeArcFlag, sweepFlag);
  const Vector2d center =
      centerNoRotation.rotate(cosRotation, sinRotation) + (currentPoint + endPoint) * 0.5;

  // Compute start angle.
  const Vector2d intersectionStart = (majorAxis - centerNoRotation) / ellipseRadius;
  const Vector2d intersectionEnd = (-majorAxis - centerNoRotation) / ellipseRadius;

  double k = intersectionStart.length();
  if (NearZero(k)) {
    return *this;
  }

  k = Clamp(intersectionStart.x / k, -1.0, 1.0);
  double theta = std::acos(k);
  if (intersectionStart.y < 0.0) {
    theta = -theta;
  }

  // Compute deltaTheta.
  k = sqrt(intersectionStart.lengthSquared() * intersectionEnd.lengthSquared());
  if (NearZero(k)) {
    return *this;
  }

  k = Clamp(intersectionStart.dot(intersectionEnd) / k, -1.0, 1.0);

  double deltaTheta = std::acos(k);
  if (intersectionStart.x * intersectionEnd.y - intersectionEnd.x * intersectionStart.y < 0.0) {
    deltaTheta = -deltaTheta;
  }

  if (sweepFlag && deltaTheta < 0.0) {
    deltaTheta += MathConstants<double>::kPi * 2.0;
  } else if (!sweepFlag && deltaTheta > 0.0) {
    deltaTheta -= MathConstants<double>::kPi * 2.0;
  }

  // Now draw the arc.
  const size_t numSegs =
      static_cast<size_t>(ceil(Abs(deltaTheta / (MathConstants<double>::kPi * 0.5 + 0.001))));
  Vector2d dir(cosRotation, sinRotation);
  const double thetaIncrement = deltaTheta / double(numSegs);

  // Draw segments.
  for (size_t i = 0; i < numSegs; ++i) {
    // Determine the properties of the current segment.
    const double thetaStart = theta + i * thetaIncrement;
    const double thetaEnd = theta + (i + 1) * thetaIncrement;

    const double thetaHalf = 0.5 * (thetaEnd - thetaStart);

    const double sinHalfThetaHalf = sin(thetaHalf * 0.5);
    const double t = (8.0 / 3.0) * sinHalfThetaHalf * sinHalfThetaHalf / sin(thetaHalf);

    const double cosThetaStart = std::cos(thetaStart);
    const double sinThetaStart = std::sin(thetaStart);
    const Vector2d point1 = ellipseRadius * Vector2d(cosThetaStart - t * sinThetaStart,
                                                     sinThetaStart + t * cosThetaStart);

    const double cosThetaEnd = std::cos(thetaEnd);
    const double sinThetaEnd = std::sin(thetaEnd);
    const Vector2d point3 = ellipseRadius * Vector2d(cosThetaEnd, sinThetaEnd);

    const Vector2d point2 = point3 + ellipseRadius * Vector2d(t * sinThetaEnd, -t * cosThetaEnd);

    // Draw a curve for this segment.
    curveTo(center + point1.rotate(dir.x, dir.y), center + point2.rotate(dir.x, dir.y),
            center + point3.rotate(dir.x, dir.y));
  }

  return *this;
}

PathSpline::Builder& PathSpline::Builder::closePath() {
  UTILS_RELEASE_ASSERT((moveToPointIndex_ != kNPos || !commands_.empty()) &&
                       "ClosePath without an open path");

  // Close the path, which will draw a line back to the start.
  commands_.emplace_back(CommandType::ClosePath, moveToPointIndex_);

  // Start a MoveTo, which may be replaced if a new segment is started with a MoveTo.
  commands_.emplace_back(CommandType::MoveTo, moveToPointIndex_);

  return *this;
}

PathSpline::Builder& PathSpline::Builder::ellipse(const Vector2d& center, const Vector2d& radius) {
  // Approximate an ellipse using four bezier curves.
  // 4/3 * (1 - cos(45 deg) / sin(45 deg) = 4/3 * (sqrt 2) - 1
  const double kArcMagic = 0.5522847498;

  // Start at theta = 0, or (radius.x, 0.0).
  moveTo(center + Vector2d(radius.x, 0.0));

  // First curve, to (0.0, radius.y).
  curveTo(center + Vector2d(radius.x, radius.y * kArcMagic),
          center + Vector2d(radius.x * kArcMagic, radius.y), center + Vector2d(0.0, radius.y));

  // Second curve, to (-radius.x, 0.0).
  curveTo(center + Vector2d(-radius.x * kArcMagic, radius.y),
          center + Vector2d(-radius.x, radius.y * kArcMagic), center + Vector2d(-radius.x, 0.0));

  // Third curve, to (0.0, -radius.y).
  curveTo(center + Vector2d(-radius.x, -radius.y * kArcMagic),
          center + Vector2d(-radius.x * kArcMagic, -radius.y), center + Vector2d(0.0, -radius.y));

  // Fourth curve, to (radius.x, 0.0).
  curveTo(center + Vector2d(radius.x * kArcMagic, -radius.y),
          center + Vector2d(radius.x, -radius.y * kArcMagic), center + Vector2d(radius.x, 0.0));

  closePath();
  return *this;
}

PathSpline::Builder& PathSpline::Builder::circle(const Vector2d& center, double radius) {
  Vector2d ellipseRadius(radius, radius);
  ellipse(center, ellipseRadius);
  return *this;
}

PathSpline PathSpline::Builder::build() {
  UTILS_RELEASE_ASSERT(valid_ && "Builder can only be used once");
  valid_ = false;

  // If the last command is a MoveTo, remove it.
  if (commands_.size() > 1) {
    const Command lastCommand = commands_.back();
    if (lastCommand.type == CommandType::MoveTo) {
      if (lastCommand.pointIndex > 0 && lastCommand.pointIndex + 1 == points_.size()) {
        // Remove point if it is not a deduped reference from earlier in the array.
        points_.pop_back();
      }
      commands_.pop_back();
    }
  }

  return PathSpline(std::move(points_), std::move(commands_));
}

PathSpline::PathSpline(std::vector<Vector2d>&& points, std::vector<Command>&& commands)
    : points_(std::move(points)), commands_(std::move(commands)) {}

}  // namespace donner::svg
