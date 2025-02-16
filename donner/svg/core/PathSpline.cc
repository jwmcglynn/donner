#include "donner/svg/core/PathSpline.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <optional>

#include "donner/base/MathUtils.h"
#include "donner/base/Utils.h"

namespace donner::svg {

namespace {

// Helper constants and functions used internally.

/// Tolerance for numerical calculations.
constexpr double kTolerance = 0.001;

/// Maximum recursion depth to prevent infinite recursion in some algorithms.
constexpr int kMaxRecursionDepth = 10;

/// Used as a sentinel value for the moveToPointIndex_ when no MoveTo command has been issued.
constexpr size_t kNPos = ~size_t(0);

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

std::optional<PathSpline> DecomposeArcIntoCubic(const Vector2d& startPoint,
                                                const Vector2d& endPoint, const Vector2d& radius,
                                                double rotationRadians, bool largeArcFlag,
                                                bool sweepFlag) {
  // See Appendix F.6 Elliptical arc implementation notes
  // http://www.w3.org/TR/SVG/implnote.html#ArcImplementationNotes
  const double kDistanceSqEpsilon = 1e-14;  // Chosen unscientifically to avoid a NearZero assert
                                            // in EllipseCenterForArc, should be sufficiently
                                            // large so that x^4 > DBL_EPSILON.

  if (UTILS_PREDICT_FALSE(NearZero(startPoint.distanceSquared(endPoint), kDistanceSqEpsilon))) {
    // No-op, the end point is the current position.
    return std::nullopt;
  }

  if (UTILS_PREDICT_FALSE(NearZero(radius.x) || NearZero(radius.y))) {
    // Zero radius falls back to a line segment.
    PathSpline result;
    result.moveTo(startPoint);
    result.lineTo(endPoint);
    return result;
  }

  // X-axis of the arc.
  const double sinRotation = std::sin(rotationRadians);
  const double cosRotation = std::cos(rotationRadians);

  // Rotate the extent to find the major axis.
  const Vector2d extent = (startPoint - endPoint) * 0.5;
  const Vector2d majorAxis = extent.rotate(cosRotation, -sinRotation);

  const Vector2d ellipseRadius = CorrectArcRadius(radius, majorAxis);

  const Vector2d centerNoRotation =
      EllipseCenterForArc(ellipseRadius, majorAxis, largeArcFlag, sweepFlag);
  const Vector2d center =
      centerNoRotation.rotate(cosRotation, sinRotation) + (startPoint + endPoint) * 0.5;

  // Compute start angle.
  const Vector2d intersectionStart = (majorAxis - centerNoRotation) / ellipseRadius;
  const Vector2d intersectionEnd = (-majorAxis - centerNoRotation) / ellipseRadius;

  double k = intersectionStart.length();
  if (NearZero(k)) {
    return std::nullopt;
  }

  k = Clamp(intersectionStart.x / k, -1.0, 1.0);
  const double theta = std::acos(k) * (intersectionStart.y < 0.0 ? -1.0 : 1.0);

  // Compute deltaTheta.
  k = sqrt(intersectionStart.lengthSquared() * intersectionEnd.lengthSquared());
  if (NearZero(k)) {
    return std::nullopt;
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

  // Determine the number of segments to draw the arc with curves
  const size_t numSegs =
      static_cast<size_t>(ceil(Abs(deltaTheta / (MathConstants<double>::kPi * 0.5 + 0.001))));
  const Vector2d dir(cosRotation, sinRotation);
  const double thetaIncrement = deltaTheta / double(numSegs);

  // Draw segments
  PathSpline result;
  result.moveTo(startPoint);

  for (size_t i = 0; i < numSegs; ++i) {
    // Determine the properties of the current segment.
    const double thetaStart = theta + static_cast<double>(i) * thetaIncrement;
    const double thetaEnd = theta + static_cast<double>(i + 1) * thetaIncrement;

    const double thetaHalf = 0.5 * (thetaEnd - thetaStart);

    const double sinHalfThetaHalf = sin(thetaHalf * 0.5);
    const double t = (8.0 / 3.0) * sinHalfThetaHalf * sinHalfThetaHalf / sin(thetaHalf);

    const double cosThetaStart = std::cos(thetaStart);
    const double sinThetaStart = std::sin(thetaStart);
    const Vector2d p0 = ellipseRadius * Vector2d(cosThetaStart - t * sinThetaStart,
                                                 sinThetaStart + t * cosThetaStart);

    const double cosThetaEnd = std::cos(thetaEnd);
    const double sinThetaEnd = std::sin(thetaEnd);
    const Vector2d p2 = ellipseRadius * Vector2d(cosThetaEnd, sinThetaEnd);

    const Vector2d p1 = p2 + ellipseRadius * Vector2d(t * sinThetaEnd, -t * cosThetaEnd);

    // Draw a curve for this segment.
    result.curveTo(center + p0.rotate(dir.x, dir.y), center + p1.rotate(dir.x, dir.y),
                   center + p2.rotate(dir.x, dir.y));
  }

  return result;
}

/**
 * Subdivide a cubic Bézier curve into two halves and measure the length.
 *
 * Recursively subdivides the curve until the flatness criterion is met, then
 * approximates the length by summing the lengths of the control polygon.
 *
 * @param points The control points of the cubic Bézier curve.
 * @param tolerance Tolerance for flatness criterion.
 * @param depth Current recursion depth.
 * @return Approximate length of the curve segment.
 */
double SubdivideAndMeasureCubic(const std::array<Vector2d, 4>& points, double tolerance,
                                int depth = 0) {
  if (depth > kMaxRecursionDepth) {
    return (points[0] - points[3]).length();
  }

  // Calculate chord length and control net length
  const double chordLength = (points[3] - points[0]).length();
  const double netLength = (points[1] - points[0]).length() + (points[2] - points[1]).length() +
                           (points[3] - points[2]).length();

  // If the difference is within tolerance, return average
  if ((netLength - chordLength) <= tolerance) {
    return (netLength + chordLength) / 2.0;
  }

  // Subdivide the curve
  const Vector2d p01 = (points[0] + points[1]) * 0.5;
  const Vector2d p12 = (points[1] + points[2]) * 0.5;
  const Vector2d p23 = (points[2] + points[3]) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  const std::array<Vector2d, 4> left = {points[0], p01, p012, p0123};
  const std::array<Vector2d, 4> right = {p0123, p123, p23, points[3]};

  // Recursively calculate lengths
  return SubdivideAndMeasureCubic(left, tolerance, depth + 1) +
         SubdivideAndMeasureCubic(right, tolerance, depth + 1);
}

/**
 * Internal helper function to compute miter joins and update the bounding box.
 *
 * @param box Bounding box to update.
 * @param currentPoint Current point in the path, where the lines intersect.
 * @param tangent0 Tangent vector at the start of the join, not normalized.
 * @param tangent1 Tangent vector at the end of the join, not normalized.
 * @param strokeWidth Width of the stroke.
 * @param miterLimit Miter limit.
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

/**
 * Calculate the distance from a point to a line segment.
 *
 * @param p Point to test.
 * @param a Start point of the line segment.
 * @param b End point of the line segment.
 * @return Distance from the point to the line segment.
 */
double DistanceFromPointToLine(const Vector2d& p, const Vector2d& a, const Vector2d& b) {
  const Vector2d ab = b - a;
  const Vector2d ap = p - a;
  const double abLengthSquared = ab.lengthSquared();
  if (NearZero(abLengthSquared)) {
    // 'a' and 'b' are the same point
    return ap.length();
  }
  const double t = Clamp(ap.dot(ab) / abLengthSquared, 0.0, 1.0);
  Vector2d projection = a + t * ab;
  return (p - projection).length();
}

/**
 * Determine if a cubic Bézier curve is flat enough for approximation.
 *
 * @param p0 Start point of the curve.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point of the curve.
 * @param tolerance Tolerance for flatness.
 * @return True if the curve is flat enough, false otherwise.
 */
bool IsCurveFlatEnough(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                       const Vector2d& p3, double tolerance) {
  // Use the distance from the control points to the line as a flatness measure
  double d1 = DistanceFromPointToLine(p1, p0, p3);
  double d2 = DistanceFromPointToLine(p2, p0, p3);
  return (d1 <= tolerance) && (d2 <= tolerance);
}

/**
 * Calculate the winding number contribution of a line segment.
 *
 * @param p0 Start point of the segment.
 * @param p1 End point of the segment.
 * @param point Point to test.
 * @return Winding number contribution.
 */
int WindingNumberContribution(const Vector2d& p0, const Vector2d& p1, const Vector2d& point) {
  if (p0.y <= point.y) {
    if (p1.y > point.y && ((p1 - p0).cross(point - p0)) > 0) {
      return 1;
    }
  } else {
    if (p1.y <= point.y && ((p1 - p0).cross(point - p0)) < 0) {
      return -1;
    }
  }
  return 0;
}

/**
 * Recursively compute the winding number contribution of a cubic Bézier curve.
 *
 * @param p0 Start point of the curve.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point of the curve.
 * @param point Point to test.
 * @param tolerance Tolerance for flatness.
 * @param depth Current recursion depth.
 * @return Winding number contribution.
 */
int WindingNumberContributionCurve(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                   const Vector2d& p3, const Vector2d& point, double tolerance,
                                   int depth = 0) {
  if (depth > kMaxRecursionDepth || IsCurveFlatEnough(p0, p1, p2, p3, tolerance)) {
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
  int winding = WindingNumberContributionCurve(p0, p01, p012, p0123, point, tolerance, depth + 1);
  winding += WindingNumberContributionCurve(p0123, p123, p23, p3, point, tolerance, depth + 1);
  return winding;
}

/**
 * Check if a point is on a cubic Bézier curve within a given tolerance.
 *
 * @param point Point to check.
 * @param p0 Start point of the curve.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point of the curve.
 * @param tolerance Tolerance for proximity.
 * @param depth Current recursion depth.
 * @return True if the point is on the curve, false otherwise.
 */
bool IsPointOnCubicBezier(const Vector2d& point, const Vector2d& p0, const Vector2d& p1,
                          const Vector2d& p2, const Vector2d& p3, double tolerance, int depth = 0) {
  if (depth > kMaxRecursionDepth || IsCurveFlatEnough(p0, p1, p2, p3, tolerance)) {
    return DistanceFromPointToLine(point, p0, p3) <= tolerance;
  }

  // Subdivide the curve and check each segment
  const Vector2d p01 = (p0 + p1) * 0.5;
  const Vector2d p12 = (p1 + p2) * 0.5;
  const Vector2d p23 = (p2 + p3) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  // Recursively check subdivisions
  return IsPointOnCubicBezier(point, p0, p01, p012, p0123, tolerance, depth + 1) ||
         IsPointOnCubicBezier(point, p0123, p123, p23, p3, tolerance, depth + 1);
}

/**
 * Interpolates between two tangents and returns a vector that represents the halfway direction.
 * If the tangents are opposite, returns a perpendicular vector.
 *
 * @param prevTangent The tangent vector at the previous position.
 * @param nextTangent The tangent vector at the current position.
 * @return Interpolated tangent vector.
 */
Vector2d InterpolateTangents(const Vector2d& prevTangent, const Vector2d& nextTangent) {
  const Vector2d sum = prevTangent + nextTangent;

  if (!NearZero(sum.lengthSquared())) {
    // If the tangents are not opposite, normalize the sum to get the halfway tangent
    return sum.normalize();
  } else {
    // If the tangents are opposite, choose a perpendicular vector
    // Rotate prevTangent by 90 degrees counter-clockwise
    return Vector2d(prevTangent.y, -prevTangent.x);
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, PathSpline::CommandType type) {
  switch (type) {
    case PathSpline::CommandType::MoveTo: os << "MoveTo"; break;
    case PathSpline::CommandType::LineTo: os << "LineTo"; break;
    case PathSpline::CommandType::CurveTo: os << "CurveTo"; break;
    case PathSpline::CommandType::ClosePath: os << "ClosePath"; break;
    default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
  }

  return os;
}

std::ostream& operator<<(std::ostream& os, const PathSpline::Command& command) {
  return os << "Command {" << command.type << ", " << command.pointIndex << "}";
}

void PathSpline::moveTo(const Vector2d& point) {
  // As an optimization, if the last command was a MoveTo replace it with the new point.
  if (!commands_.empty() && commands_.back().type == CommandType::MoveTo) {
    Command& lastCommand = commands_.back();

    // The last moveTo should be a unique point, so we can safely replace it.
    assert(lastCommand.pointIndex + 1 == points_.size());

    points_[lastCommand.pointIndex] = point;
  } else {
    const size_t pointIndex = points_.size();
    const size_t commandIndex = commands_.size();
    points_.push_back(point);
    commands_.emplace_back(CommandType::MoveTo, pointIndex);

    moveToPointIndex_ = pointIndex;
    currentSegmentStartCommandIndex_ = commandIndex;
  }

  mayAutoReopen_ = false;
}

void PathSpline::lineTo(const Vector2d& point) {
  UTILS_RELEASE_ASSERT_MSG(moveToPointIndex_ != kNPos, "lineTo without calling moveTo first");

  maybeAutoReopen();

  const size_t index = points_.size();

  points_.push_back(point);
  commands_.push_back({CommandType::LineTo, index});
}

void PathSpline::curveTo(const Vector2d& control1, const Vector2d& control2,
                         const Vector2d& endPoint) {
  UTILS_RELEASE_ASSERT_MSG(moveToPointIndex_ != kNPos, "curveTo without calling moveTo first");

  maybeAutoReopen();

  const size_t index = points_.size();
  points_.push_back(control1);
  points_.push_back(control2);
  points_.push_back(endPoint);
  commands_.push_back({CommandType::CurveTo, index});
}

void PathSpline::arcTo(const Vector2d& radius, double rotationRadians, bool largeArcFlag,
                       bool sweepFlag, const Vector2d& endPoint) {
  UTILS_RELEASE_ASSERT_MSG(moveToPointIndex_ != kNPos, "arcTo without calling MoveTo first");

  if (auto maybePath = DecomposeArcIntoCubic(currentPoint(), endPoint, radius, rotationRadians,
                                             largeArcFlag, sweepFlag)) {
    appendJoin(*maybePath, /*asInternalPath=*/true);
  }
}

void PathSpline::closePath() {
  UTILS_RELEASE_ASSERT_MSG(moveToPointIndex_ != kNPos || !commands_.empty(),
                           "ClosePath without an open path");

  assert(currentSegmentStartCommandIndex_ != kNPos);

  // Close the path, which will draw a line back to the start.
  const size_t commandIndex = commands_.size();
  commands_.emplace_back(CommandType::ClosePath, moveToPointIndex_);

  commands_[currentSegmentStartCommandIndex_].closePathIndex = commandIndex;

  mayAutoReopen_ = true;
  currentSegmentStartCommandIndex_ = kNPos;
}

void PathSpline::ellipse(const Vector2d& center, const Vector2d& radius) {
  // Approximate an ellipse using four cubic Bézier curves.
  // Magic constant for approximation
  const double kappa = 0.552284749831;  // (4 * (sqrt(2) - 1)) / 3

  moveTo(center + Vector2d(radius.x, 0));

  curveTo(center + Vector2d(radius.x, radius.y * kappa),
          center + Vector2d(radius.x * kappa, radius.y), center + Vector2d(0, radius.y));

  curveTo(center + Vector2d(-radius.x * kappa, radius.y),
          center + Vector2d(-radius.x, radius.y * kappa), center + Vector2d(-radius.x, 0));

  curveTo(center + Vector2d(-radius.x, -radius.y * kappa),
          center + Vector2d(-radius.x * kappa, -radius.y), center + Vector2d(0, -radius.y));

  curveTo(center + Vector2d(radius.x * kappa, -radius.y),
          center + Vector2d(radius.x, -radius.y * kappa), center + Vector2d(radius.x, 0));

  closePath();
}

void PathSpline::circle(const Vector2d& center, double radius) {
  ellipse(center, Vector2d(radius, radius));
}

void PathSpline::appendJoin(const PathSpline& spline, bool asInternalPath) {
  if (spline.empty()) {
    return;
  }

  // Record the current size of points_ to adjust indices
  const size_t pointOffset = points_.size();

  // Append the points from the spline, skipping the first point.
  points_.insert(points_.end(), spline.points_.begin() + 1, spline.points_.end());

  // Append the commands, adjusting the point indices
  for (size_t i = 1; i < spline.commands_.size(); ++i) {
    Command newCmd = spline.commands_[i];
    assert(newCmd.pointIndex != 0 &&
           "Point 0 unexpectedly used, this should be skipped by skipping the moveTo");

    newCmd.pointIndex = newCmd.pointIndex - 1 + pointOffset;

    // Set isInternalPoint if asInternalPath is true
    if (asInternalPath && i != spline.commands_.size() - 1) {
      newCmd.isInternalPoint = true;
    }

    commands_.push_back(newCmd);

    // Update moveToPointIndex_ if the command is MoveTo
    if (newCmd.type == CommandType::MoveTo) {
      moveToPointIndex_ = newCmd.pointIndex;
    }
  }
}

double PathSpline::pathLength() const {
  double totalLength = 0.0;
  Vector2d startPoint;

  for (const Command& command : commands_) {
    switch (command.type) {
      case CommandType::MoveTo: {
        startPoint = points_[command.pointIndex];
        break;
      }
      case CommandType::ClosePath: [[fallthrough]];
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
      default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
    }
  }

  return totalLength;
}

Vector2d PathSpline::currentPoint() const {
  UTILS_RELEASE_ASSERT(!commands_.empty());

  return endPoint(commands_.size() - 1);
}

Boxd PathSpline::bounds() const {
  return transformedBounds(Transformd());
}

Boxd PathSpline::transformedBounds(const Transformd& pathFromTarget) const {
  UTILS_RELEASE_ASSERT(!empty());

  Boxd box = Boxd::CreateEmpty(pathFromTarget.transformPosition(points_.front()));
  Vector2d currentPoint;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];
    switch (command.type) {
      case CommandType::MoveTo: [[fallthrough]];
      case CommandType::LineTo: [[fallthrough]];
      case CommandType::ClosePath: {
        currentPoint = points_[command.pointIndex];
        box.addPoint(pathFromTarget.transformPosition(currentPoint));
        break;
      }

      case CommandType::CurveTo: {
        const Vector2d& startPoint = currentPoint;
        const Vector2d& controlPoint1 = points_[command.pointIndex];
        const Vector2d& controlPoint2 = points_[command.pointIndex + 1];
        const Vector2d& endPoint = points_[command.pointIndex + 2];

        box.addPoint(pathFromTarget.transformPosition(startPoint));
        box.addPoint(pathFromTarget.transformPosition(endPoint));
        currentPoint = endPoint;

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
            3.0 * (-startPoint + 3.0 * controlPoint1 - 3.0 * controlPoint2 + endPoint);
        const Vector2d b = 6.0 * (startPoint + controlPoint2 - 2.0 * controlPoint1);
        const Vector2d c = 3.0 * (-startPoint + controlPoint1);

        // Add the x extrema.
        if (NearZero(a.x)) {
          if (!NearZero(b.x)) {
            const double t = -c.x / b.x;
            if (t >= 0.0 && t <= 1.0) {
              box.addPoint(pathFromTarget.transformPosition(pointAt(i, t)));
            }
          }
        } else {
          // Solve using the quadratic formula.
          const QuadraticSolution<double> res = SolveQuadratic(a.x, b.x, c.x);

          if (res.hasSolution) {
            if (res.solution[0] >= 0.0 && res.solution[0] <= 1.0) {
              box.addPoint(pathFromTarget.transformPosition(pointAt(i, res.solution[0])));
            }

            if (res.solution[1] >= 0.0 && res.solution[1] <= 1.0) {
              box.addPoint(pathFromTarget.transformPosition(pointAt(i, res.solution[1])));
            }
          }
        }

        // Add the y extrema.
        if (NearZero(a.y)) {
          if (!NearZero(b.y)) {
            const double t = -c.y / b.y;
            if (t >= 0.0 && t <= 1.0) {
              box.addPoint(pathFromTarget.transformPosition(pointAt(i, t)));
            }
          }
        } else {
          // Solve using the quadratic formula.
          QuadraticSolution<double> res = SolveQuadratic(a.y, b.y, c.y);

          if (res.hasSolution) {
            if (res.solution[0] >= 0.0 && res.solution[0] <= 1.0) {
              box.addPoint(pathFromTarget.transformPosition(pointAt(i, res.solution[0])));
            }

            if (res.solution[1] >= 0.0 && res.solution[1] <= 1.0) {
              box.addPoint(pathFromTarget.transformPosition(pointAt(i, res.solution[1])));
            }
          }
        }

        break;
      }
      default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
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

      default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
    }
  }

  return box;
}

Vector2d PathSpline::pointAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0.0 && t <= 1.0 && "t out of range");

  const Command& command = commands_.at(index);

  switch (command.type) {
    case CommandType::MoveTo: return startPoint(index);
    case CommandType::LineTo: [[fallthrough]];
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
    default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
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

    case CommandType::LineTo: [[fallthrough]];
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

      const Vector2d derivative = 3.0 * (rev_t * rev_t * p_1_0      // (1 - t)^2 * (P_1 - P_0)
                                         + 2.0 * t * rev_t * p_2_1  // (1 - t) * t * (P_2 - P_1)
                                         + t * t * p_3_2            // t^2 * (P_3 - P_2)
                                        );

      if (NearZero(derivative.lengthSquared())) {
        // First derivative is zero, which indicates two control points are the same (a degenerate
        // curve). Adjust the t value and try again.
        double adjustedT = t;
        if (NearEquals(t, 0.0, 0.000001)) {
          adjustedT = 0.01;
        } else if (NearEquals(t, 1.0, 0.000001)) {
          adjustedT = 0.99;
        } else {
          return derivative;
        }

        return tangentAt(index, adjustedT);
      } else {
        return derivative;
      }
    }
    default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
  }
}

Vector2d PathSpline::normalAt(size_t index, double t) const {
  const Vector2d tangent = tangentAt(index, t);
  return Vector2d(-tangent.y, tangent.x);
}

std::vector<PathSpline::Vertex> PathSpline::vertices() const {
  std::vector<Vertex> vertices;
  std::optional<size_t> openPathCommand;
  size_t closePathIndex = size_t(-1);
  bool justMoved = false;
  bool wasInternalPoint = false;

  // Create vertices at the start of each segment.
  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];
    const bool shouldSkip = wasInternalPoint;
    wasInternalPoint = command.isInternalPoint;

    if (shouldSkip) {
      continue;
    }

    if (command.type == CommandType::MoveTo) {
      if (openPathCommand) {
        assert(i > 0);

        // Place a vertex at the previous point. For open subpaths, the orientation is the
        // direction of the line.
        const Vector2d point = pointAt(i - 1, 1.0);
        const Vector2d orientation = tangentAt(i - 1, 1.0).normalize();
        vertices.push_back(Vertex{point, orientation});
      }

      openPathCommand = i;
      closePathIndex = command.closePathIndex;
      justMoved = true;

    } else if (command.type == CommandType::ClosePath) {
      // If this ClosePath draws a line back to the starting point, place a vertex at the
      // starting point. Since this is a closed subpath, the orientation is halfway between the
      // starting point and the end point.
      assert(openPathCommand);
      assert(i > 0);

      const Vector2d startPoint = pointAt(i - 1, 1.0);
      const Vector2d endPoint = pointAt(*openPathCommand, 0.0);

      // If the line is very short, we don't want to place a vertex at the start point, as it
      // will be very close to the end point.
      if (!NearZero((startPoint - endPoint).lengthSquared())) {
        const Vector2d prevTangent = tangentAt(i - 1, 1.0).normalize();
        const Vector2d nextTangent = tangentAt(i, 0.0).normalize();

        const Vector2d orientationStart = InterpolateTangents(prevTangent, nextTangent);
        vertices.push_back(Vertex{startPoint, orientationStart});
      }

      // Place a vertex at the end point. For closed subpaths, the orientation is halfway between
      // the start point and the end point.
      {
        const Vector2d prevTangent = tangentAt(i, 1.0).normalize();
        const Vector2d nextTangent = tangentAt(*openPathCommand, 0.0).normalize();

        const Vector2d orientationEnd = InterpolateTangents(prevTangent, nextTangent);
        vertices.push_back(Vertex{endPoint, orientationEnd});
      }

      openPathCommand = std::nullopt;
      justMoved = false;
    } else {
      // This is a LineTo or CurveTo, place a vertex at the start point.
      assert(i > 0);

      const Vector2d startPoint = pointAt(i, 0.0);
      const Vector2d startOrientation = tangentAt(i, 0.0).normalize();

      if (justMoved) {
        // If this is the first point of a new subpath, we need to orient the anchor differently
        // if the subpath is closed.
        if (closePathIndex != size_t(-1)) {
          // For closed subpaths, the orientation is halfway between the starting point and the
          // end.
          const Vector2d closeOrientation = tangentAt(closePathIndex, 1.0).normalize();
          vertices.push_back(
              Vertex{startPoint, InterpolateTangents(closeOrientation, startOrientation)});
        } else {
          // For open subpaths, the orientation is the direction of the line.
          vertices.push_back(Vertex{startPoint, startOrientation});
        }
      } else {
        // Otherwise place a vertex at the start with the orientation halfway between the start of
        // this segment and end of the previous.
        const Vector2d prevOrientation = tangentAt(i - 1, 1.0).normalize();

        vertices.push_back(
            Vertex{startPoint, InterpolateTangents(prevOrientation, startOrientation)});
      }

      justMoved = false;
    }
  }

  // This is an open path, place the final vertex.
  if (openPathCommand && commands_.size() > 1) {
    const Vector2d point = pointAt(commands_.size() - 1, 1.0);
    const Vector2d orientation = tangentAt(commands_.size() - 1, 1.0).normalize();
    vertices.push_back(Vertex{point, orientation});
  }

  return vertices;
}

bool PathSpline::isInside(const Vector2d& point, FillRule fillRule) const {
  const double kIsInsideTolerance = 0.1;

  int windingNumber = 0;
  Vector2d currentPoint;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];
    switch (command.type) {
      case CommandType::MoveTo: {
        currentPoint = points_[command.pointIndex];
        break;
      }

      case CommandType::ClosePath: [[fallthrough]];
      case CommandType::LineTo: {
        const Vector2d& endPoint = points_[command.pointIndex];
        if (DistanceFromPointToLine(point, currentPoint, endPoint) <= kIsInsideTolerance) {
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
                                 kIsInsideTolerance)) {
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

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
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

      default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
    }
  }

  return false;
}

std::ostream& operator<<(std::ostream& os, const PathSpline& spline) {
  os << "[\n";

  // Iterate through the commands and output a summary
  for (size_t i = 0; i < spline.commands().size(); ++i) {
    const PathSpline::Command& cmd = spline.commands()[i];

    os << "  " << i << ": " << spline.commands()[i].type << " ";
    if (cmd.type == PathSpline::CommandType::MoveTo) {
      os << spline.points()[cmd.pointIndex];
    } else if (cmd.type == PathSpline::CommandType::LineTo) {
      os << spline.points()[cmd.pointIndex];
    } else if (cmd.type == PathSpline::CommandType::CurveTo) {
      os << spline.points()[cmd.pointIndex] << ", " << spline.points()[cmd.pointIndex + 1] << ", "
         << spline.points()[cmd.pointIndex + 2];
    }
    os << ",\n";
  }

  return os << "]\n";
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
      case CommandType::MoveTo: [[fallthrough]];
      case CommandType::LineTo: [[fallthrough]];
      case CommandType::ClosePath: {
        return points_[prevCommand.pointIndex];
      }
      case CommandType::CurveTo: {
        return points_[prevCommand.pointIndex + 2];
      }
      default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
    }
  }
}

Vector2d PathSpline::endPoint(size_t index) const {
  assert(index < commands_.size() && "index out of range");

  const Command& currentCommand = commands_[index];
  switch (currentCommand.type) {
    case CommandType::MoveTo: [[fallthrough]];
    case CommandType::LineTo: [[fallthrough]];
    case CommandType::ClosePath: {
      return points_[currentCommand.pointIndex];
    }
    case CommandType::CurveTo: {
      return points_[currentCommand.pointIndex + 2];
    }
    default: UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
  }
}

void PathSpline::maybeAutoReopen() {
  if (mayAutoReopen_) {
    // If the path is already closed, we need to reopen it.
    const size_t commandIndex = commands_.size();
    commands_.emplace_back(CommandType::MoveTo, moveToPointIndex_);

    mayAutoReopen_ = false;
    currentSegmentStartCommandIndex_ = commandIndex;
  }
}

}  // namespace donner::svg
