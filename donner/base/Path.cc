#include "donner/base/Path.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "donner/base/BezierUtils.h"
#include "donner/base/MathUtils.h"
#include "donner/base/Utils.h"

namespace donner {

// ============================================================================
// Path
// ============================================================================

std::ostream& operator<<(std::ostream& os, Path::Verb verb) {
  switch (verb) {
    case Path::Verb::MoveTo: return os << "MoveTo";
    case Path::Verb::LineTo: return os << "LineTo";
    case Path::Verb::QuadTo: return os << "QuadTo";
    case Path::Verb::CurveTo: return os << "CurveTo";
    case Path::Verb::ClosePath: return os << "ClosePath";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, const Path::Command& command) {
  return os << "Command {" << command.verb << ", " << command.pointIndex << "}";
}

Box2d Path::bounds() const {
  if (points_.empty()) {
    return Box2d();
  }

  Box2d box = Box2d::CreateEmpty(points_[0]);

  for (const auto& cmd : commands_) {
    switch (cmd.verb) {
      case Verb::MoveTo:
      case Verb::LineTo:
        box.addPoint(points_[cmd.pointIndex]);
        break;

      case Verb::QuadTo: {
        // Find the start point (previous command's end point or the moveTo).
        const Vector2d& start = (cmd.pointIndex >= 2) ? points_[cmd.pointIndex - 1]
                                                       : (cmd.pointIndex >= 1 ? points_[cmd.pointIndex - 1] : points_[0]);
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& end = points_[cmd.pointIndex + 1];
        const Box2d qBounds = QuadraticBounds(start, control, end);
        box.addPoint(qBounds.topLeft);
        box.addPoint(qBounds.bottomRight);
        break;
      }

      case Verb::CurveTo: {
        const Vector2d& start = (cmd.pointIndex >= 1) ? points_[cmd.pointIndex - 1] : points_[0];
        const Vector2d& c1 = points_[cmd.pointIndex];
        const Vector2d& c2 = points_[cmd.pointIndex + 1];
        const Vector2d& end = points_[cmd.pointIndex + 2];
        const Box2d cBounds = CubicBounds(start, c1, c2, end);
        box.addPoint(cBounds.topLeft);
        box.addPoint(cBounds.bottomRight);
        break;
      }

      case Verb::ClosePath:
        break;
    }
  }

  return box;
}

Box2d Path::transformedBounds(const Transform2d& transform) const {
  // Transform all points and compute bounds on the transformed path.
  // For curves, this is an approximation (transforms the control points, not the exact curve).
  // For tight bounds, transform then recompute, but for most uses this is sufficient.
  if (points_.empty()) {
    return Box2d();
  }

  // Simple approach: transform the bounding box.
  return transform.transformBox(bounds());
}

namespace {

constexpr double kPathLengthTolerance = 0.001;
constexpr int kMaxSubdivisionDepth = 10;

/// Approximate the arc length of a cubic Bezier via recursive subdivision.
double subdivideCubicLength(const std::array<Vector2d, 4>& pts, double tolerance, int depth) {
  if (depth > kMaxSubdivisionDepth) {
    return (pts[3] - pts[0]).length();
  }

  const double chordLength = (pts[3] - pts[0]).length();
  const double netLength =
      (pts[1] - pts[0]).length() + (pts[2] - pts[1]).length() + (pts[3] - pts[2]).length();

  if ((netLength - chordLength) <= tolerance) {
    return (netLength + chordLength) / 2.0;
  }

  // De Casteljau subdivision at t=0.5.
  const Vector2d p01 = (pts[0] + pts[1]) * 0.5;
  const Vector2d p12 = (pts[1] + pts[2]) * 0.5;
  const Vector2d p23 = (pts[2] + pts[3]) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  const std::array<Vector2d, 4> left = {pts[0], p01, p012, p0123};
  const std::array<Vector2d, 4> right = {p0123, p123, p23, pts[3]};

  return subdivideCubicLength(left, tolerance, depth + 1) +
         subdivideCubicLength(right, tolerance, depth + 1);
}

/// Measure the arc length of a cubic Bezier from t=0 to \p tEnd via De Casteljau split.
double measureCubicPartial(const std::array<Vector2d, 4>& pts, double tEnd, double tolerance,
                           int depth = 0) {
  if (depth > kMaxSubdivisionDepth || tEnd <= 0.0) {
    return 0.0;
  }
  if (tEnd >= 1.0) {
    return subdivideCubicLength(pts, tolerance, 0);
  }

  // De Casteljau split at tEnd to get the left sub-curve [0, tEnd].
  const auto lerp = [](const Vector2d& a, const Vector2d& b, double t) {
    return a + (b - a) * t;
  };
  const Vector2d p01 = lerp(pts[0], pts[1], tEnd);
  const Vector2d p12 = lerp(pts[1], pts[2], tEnd);
  const Vector2d p23 = lerp(pts[2], pts[3], tEnd);
  const Vector2d p012 = lerp(p01, p12, tEnd);
  const Vector2d p123 = lerp(p12, p23, tEnd);
  const Vector2d p0123 = lerp(p012, p123, tEnd);

  const std::array<Vector2d, 4> left = {pts[0], p01, p012, p0123};
  return subdivideCubicLength(left, tolerance, 0);
}

/// Binary-search for the t parameter on a cubic Bezier where arc length equals \p targetDist.
double findTForArcLength(const std::array<Vector2d, 4>& pts, double targetDist, double totalSegLen,
                         double tolerance) {
  if (targetDist <= 0.0) {
    return 0.0;
  }
  if (targetDist >= totalSegLen) {
    return 1.0;
  }

  double lo = 0.0;
  double hi = 1.0;
  double mid = targetDist / totalSegLen;  // Linear initial estimate.

  for (int iter = 0; iter < 30; ++iter) {
    const double len = measureCubicPartial(pts, mid, tolerance);
    const double error = len - targetDist;
    if (std::abs(error) < tolerance * 0.1) {
      break;
    }
    if (error > 0.0) {
      hi = mid;
    } else {
      lo = mid;
    }
    mid = (lo + hi) * 0.5;
  }

  return mid;
}

/// Evaluate a cubic Bezier at parameter \p t.
Vector2d evalCubic(const std::array<Vector2d, 4>& pts, double t) {
  const double s = 1.0 - t;
  return s * s * s * pts[0] + 3.0 * s * s * t * pts[1] + 3.0 * s * t * t * pts[2] +
         t * t * t * pts[3];
}

/// Evaluate the tangent (first derivative) of a cubic Bezier at parameter \p t.
Vector2d evalCubicTangent(const std::array<Vector2d, 4>& pts, double t) {
  const double s = 1.0 - t;
  return 3.0 * (s * s * (pts[1] - pts[0]) + 2.0 * s * t * (pts[2] - pts[1]) +
                t * t * (pts[3] - pts[2]));
}

/// Get the end point of command at index \p i.
Vector2d endPointOfCommand(const std::vector<Path::Command>& commands,
                           const std::vector<Vector2d>& points, size_t i) {
  const auto& cmd = commands[i];
  switch (cmd.verb) {
    case Path::Verb::MoveTo: return points[cmd.pointIndex];
    case Path::Verb::LineTo: return points[cmd.pointIndex];
    case Path::Verb::QuadTo: return points[cmd.pointIndex + 1];
    case Path::Verb::CurveTo: return points[cmd.pointIndex + 2];
    case Path::Verb::ClosePath: {
      // Walk backwards to find the last MoveTo.
      for (size_t j = i; j-- > 0;) {
        if (commands[j].verb == Path::Verb::MoveTo) {
          return points[commands[j].pointIndex];
        }
      }
      return points.empty() ? Vector2d() : points[0];
    }
  }
  return Vector2d();
}

/// Get the start point of command at index \p i (end point of the previous command).
Vector2d startPointOfCommand(const std::vector<Path::Command>& commands,
                             const std::vector<Vector2d>& points, size_t i) {
  if (i == 0) {
    return points.empty() ? Vector2d() : points[0];
  }
  return endPointOfCommand(commands, points, i - 1);
}

}  // namespace

double Path::pathLength() const {
  double totalLength = 0.0;
  Vector2d currentPoint;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const auto& cmd = commands_[i];
    switch (cmd.verb) {
      case Verb::MoveTo: {
        currentPoint = points_[cmd.pointIndex];
        break;
      }
      case Verb::LineTo: {
        const Vector2d& endPt = points_[cmd.pointIndex];
        totalLength += currentPoint.distance(endPt);
        currentPoint = endPt;
        break;
      }
      case Verb::QuadTo: {
        // Elevate quadratic to cubic for arc length measurement.
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& endPt = points_[cmd.pointIndex + 1];
        // Quadratic-to-cubic elevation: cubic c1 = start + 2/3*(control - start),
        // cubic c2 = end + 2/3*(control - end).
        const Vector2d c1 = currentPoint + (control - currentPoint) * (2.0 / 3.0);
        const Vector2d c2 = endPt + (control - endPt) * (2.0 / 3.0);
        const std::array<Vector2d, 4> cubicPts = {currentPoint, c1, c2, endPt};
        totalLength += subdivideCubicLength(cubicPts, kPathLengthTolerance, 0);
        currentPoint = endPt;
        break;
      }
      case Verb::CurveTo: {
        const std::array<Vector2d, 4> cubicPts = {currentPoint, points_[cmd.pointIndex],
                                                   points_[cmd.pointIndex + 1],
                                                   points_[cmd.pointIndex + 2]};
        totalLength += subdivideCubicLength(cubicPts, kPathLengthTolerance, 0);
        currentPoint = points_[cmd.pointIndex + 2];
        break;
      }
      case Verb::ClosePath: {
        // ClosePath draws a line back to the last MoveTo point.
        Vector2d moveToPoint;
        for (size_t j = i; j-- > 0;) {
          if (commands_[j].verb == Verb::MoveTo) {
            moveToPoint = points_[commands_[j].pointIndex];
            break;
          }
        }
        totalLength += currentPoint.distance(moveToPoint);
        currentPoint = moveToPoint;
        break;
      }
    }
  }

  return totalLength;
}

Path::PointOnPath Path::pointAtArcLength(double distance) const {
  if (commands_.empty() || distance < 0.0) {
    return {{}, {}, 0.0, false};
  }

  double accumulated = 0.0;
  Vector2d currentPoint;
  Vector2d subpathStart;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const auto& cmd = commands_[i];

    switch (cmd.verb) {
      case Verb::MoveTo: {
        currentPoint = points_[cmd.pointIndex];
        subpathStart = currentPoint;
        break;
      }

      case Verb::LineTo: {
        const Vector2d& endPt = points_[cmd.pointIndex];
        const double segLen = currentPoint.distance(endPt);

        if (accumulated + segLen >= distance) {
          const double remaining = distance - accumulated;
          const double t = (segLen > 0.0) ? remaining / segLen : 0.0;
          const Vector2d pt = currentPoint + (endPt - currentPoint) * t;
          const Vector2d tang = endPt - currentPoint;
          return {pt, tang, std::atan2(tang.y, tang.x), true};
        }

        accumulated += segLen;
        currentPoint = endPt;
        break;
      }

      case Verb::QuadTo: {
        // Elevate quadratic to cubic for arc length measurement and interpolation.
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& endPt = points_[cmd.pointIndex + 1];
        const Vector2d c1 = currentPoint + (control - currentPoint) * (2.0 / 3.0);
        const Vector2d c2 = endPt + (control - endPt) * (2.0 / 3.0);
        const std::array<Vector2d, 4> cubicPts = {currentPoint, c1, c2, endPt};
        const double segLen = subdivideCubicLength(cubicPts, kPathLengthTolerance, 0);

        if (accumulated + segLen >= distance) {
          const double remaining = distance - accumulated;
          const double t = findTForArcLength(cubicPts, remaining, segLen, kPathLengthTolerance);
          const Vector2d pt = evalCubic(cubicPts, t);
          const Vector2d tang = evalCubicTangent(cubicPts, t);
          return {pt, tang, std::atan2(tang.y, tang.x), true};
        }

        accumulated += segLen;
        currentPoint = endPt;
        break;
      }

      case Verb::CurveTo: {
        const std::array<Vector2d, 4> cubicPts = {currentPoint, points_[cmd.pointIndex],
                                                   points_[cmd.pointIndex + 1],
                                                   points_[cmd.pointIndex + 2]};
        const double segLen = subdivideCubicLength(cubicPts, kPathLengthTolerance, 0);

        if (accumulated + segLen >= distance) {
          const double remaining = distance - accumulated;
          const double t = findTForArcLength(cubicPts, remaining, segLen, kPathLengthTolerance);
          const Vector2d pt = evalCubic(cubicPts, t);
          const Vector2d tang = evalCubicTangent(cubicPts, t);
          return {pt, tang, std::atan2(tang.y, tang.x), true};
        }

        accumulated += segLen;
        currentPoint = points_[cmd.pointIndex + 2];
        break;
      }

      case Verb::ClosePath: {
        // ClosePath draws a line back to the last MoveTo point.
        const double segLen = currentPoint.distance(subpathStart);

        if (accumulated + segLen >= distance) {
          const double remaining = distance - accumulated;
          const double t = (segLen > 0.0) ? remaining / segLen : 0.0;
          const Vector2d pt = currentPoint + (subpathStart - currentPoint) * t;
          const Vector2d tang = subpathStart - currentPoint;
          return {pt, tang, std::atan2(tang.y, tang.x), true};
        }

        accumulated += segLen;
        currentPoint = subpathStart;
        break;
      }
    }
  }

  // Distance exceeds path length — return endpoint.
  return {currentPoint, {}, 0.0, false};
}

Vector2d Path::pointAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0.0 && t <= 1.0 && "t out of range");

  const Command& cmd = commands_.at(index);

  switch (cmd.verb) {
    case Verb::MoveTo: return points_[cmd.pointIndex];

    case Verb::LineTo: [[fallthrough]];
    case Verb::ClosePath: {
      const Vector2d start = startPointOfCommand(commands_, points_, index);
      const double rev_t = 1.0 - t;
      return rev_t * start + t * points_[cmd.pointIndex];
    }

    case Verb::QuadTo: {
      const Vector2d start = startPointOfCommand(commands_, points_, index);
      const double rev_t = 1.0 - t;
      return rev_t * rev_t * start                          // (1 - t)^2 * P0
             + 2.0 * rev_t * t * points_[cmd.pointIndex]    // 2(1 - t)t * P1
             + t * t * points_[cmd.pointIndex + 1];          // t^2 * P2
    }

    case Verb::CurveTo: {
      const Vector2d start = startPointOfCommand(commands_, points_, index);
      const double rev_t = 1.0 - t;
      return rev_t * rev_t * rev_t * start                              // (1 - t)^3 * P0
             + 3.0 * t * rev_t * rev_t * points_[cmd.pointIndex]        // 3t(1 - t)^2 * P1
             + 3.0 * t * t * rev_t * points_[cmd.pointIndex + 1]        // 3t^2(1 - t) * P2
             + t * t * t * points_[cmd.pointIndex + 2];                  // t^3 * P3
    }
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

Vector2d Path::tangentAt(size_t index, double t) const {
  assert(index < commands_.size() && "index out of range");
  assert(t >= 0.0 && t <= 1.0 && "t out of range");

  const Command& cmd = commands_.at(index);

  switch (cmd.verb) {
    case Verb::MoveTo:
      if (index + 1 < commands_.size()) {
        return tangentAt(index + 1, 0.0);
      } else {
        return Vector2d::Zero();
      }

    case Verb::LineTo: [[fallthrough]];
    case Verb::ClosePath:
      return points_[cmd.pointIndex] - startPointOfCommand(commands_, points_, index);

    case Verb::QuadTo: {
      const Vector2d start = startPointOfCommand(commands_, points_, index);
      const double rev_t = 1.0 - t;

      // Derivative of quadratic Bézier: 2[(1-t)(P1 - P0) + t(P2 - P1)]
      const Vector2d p_1_0 = points_[cmd.pointIndex] - start;
      const Vector2d p_2_1 = points_[cmd.pointIndex + 1] - points_[cmd.pointIndex];

      Vector2d derivative = 2.0 * (rev_t * p_1_0 + t * p_2_1);

      if (NearZero(derivative.lengthSquared())) {
        // Degenerate: control point coincides with an endpoint.
        derivative = points_[cmd.pointIndex + 1] - start;
      }
      return derivative;
    }

    case Verb::CurveTo: {
      const Vector2d start = startPointOfCommand(commands_, points_, index);
      const double rev_t = 1.0 - t;

      // Derivative of cubic Bézier:
      // 3[(1-t)^2 (P1 - P0) + 2(1-t)t(P2 - P1) + t^2(P3 - P2)]
      const Vector2d p_1_0 = points_[cmd.pointIndex] - start;
      const Vector2d p_2_1 = points_[cmd.pointIndex + 1] - points_[cmd.pointIndex];
      const Vector2d p_3_2 = points_[cmd.pointIndex + 2] - points_[cmd.pointIndex + 1];

      const Vector2d derivative = 3.0 * (rev_t * rev_t * p_1_0        // (1 - t)^2 * (P1 - P0)
                                          + 2.0 * t * rev_t * p_2_1   // 2t(1-t) * (P2 - P1)
                                          + t * t * p_3_2              // t^2 * (P3 - P2)
                                         );

      if (NearZero(derivative.lengthSquared())) {
        // First derivative is zero — degenerate curve with coincident control points.
        // Adjust t slightly and retry.
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
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

Vector2d Path::normalAt(size_t index, double t) const {
  const Vector2d tangent = tangentAt(index, t);
  return Vector2d(-tangent.y, tangent.x);
}

namespace {

/**
 * Expand a bounding box to account for miter join extensions at a path vertex.
 *
 * @param box Bounding box to expand.
 * @param currentPoint The join vertex.
 * @param tangent0 Tangent vector at the end of the incoming segment (un-normalized).
 * @param tangent1 Tangent vector at the start of the outgoing segment (un-normalized).
 * @param strokeWidth Width of the stroke.
 * @param miterLimit Miter limit.
 */
void ComputeMiter(Box2d& box, const Vector2d& currentPoint, const Vector2d& tangent0,
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

}  // namespace

Box2d Path::strokeMiterBounds(double strokeWidth, double miterLimit) const {
  UTILS_RELEASE_ASSERT(!empty());
  assert(strokeWidth > 0.0);
  assert(miterLimit >= 0.0);

  Box2d box = Box2d::CreateEmpty(points_.front());
  Vector2d current;

  constexpr size_t kNPos = ~size_t(0);
  size_t lastIndex = kNPos;
  size_t lastMoveToIndex = kNPos;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& cmd = commands_[i];

    switch (cmd.verb) {
      case Verb::MoveTo: {
        current = points_[cmd.pointIndex];
        box.addPoint(current);

        lastIndex = kNPos;
        lastMoveToIndex = i;
        break;
      }
      case Verb::ClosePath: {
        if (lastIndex != kNPos) {
          // For ClosePath, start with a standard line segment.
          const Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          const Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
          current = points_[cmd.pointIndex];

          // Then "join" it to the first segment of the subpath.
          const Vector2d joinTangent = tangentAt(lastMoveToIndex, 0.0);
          ComputeMiter(box, current, tangent, joinTangent, strokeWidth, miterLimit);
        }

        lastIndex = kNPos;
        break;
      }
      case Verb::LineTo: {
        if (lastIndex != kNPos) {
          const Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          const Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
        }

        current = points_[cmd.pointIndex];
        box.addPoint(current);
        lastIndex = i;
        break;
      }
      case Verb::QuadTo: {
        if (lastIndex != kNPos) {
          const Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          const Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
        }

        current = points_[cmd.pointIndex + 1];
        box.addPoint(current);
        lastIndex = i;
        break;
      }
      case Verb::CurveTo: {
        if (lastIndex != kNPos) {
          const Vector2d lastTangent = tangentAt(lastIndex, 1.0);
          const Vector2d tangent = tangentAt(i, 0.0);

          ComputeMiter(box, current, lastTangent, tangent, strokeWidth, miterLimit);
        }

        current = points_[cmd.pointIndex + 2];
        box.addPoint(current);
        lastIndex = i;
        break;
      }
    }
  }

  return box;
}

namespace {

// ---- Hit-testing helpers ----

constexpr double kHitTestTolerance = 0.001;
constexpr int kHitTestMaxDepth = 10;

/// Distance from point \p p to the line segment (a, b).
double DistanceFromPointToLine(const Vector2d& p, const Vector2d& a, const Vector2d& b) {
  const Vector2d ab = b - a;
  const Vector2d ap = p - a;
  const double abLenSq = ab.lengthSquared();
  if (NearZero(abLenSq)) {
    return ap.length();
  }
  const double t = Clamp(ap.dot(ab) / abLenSq, 0.0, 1.0);
  return (p - (a + t * ab)).length();
}

/// True when both control points are close to the chord from p0 to p3.
bool IsCurveFlatEnough(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                       const Vector2d& p3, double tolerance) {
  return DistanceFromPointToLine(p1, p0, p3) <= tolerance &&
         DistanceFromPointToLine(p2, p0, p3) <= tolerance;
}

/// Winding-number contribution of a single line segment for a test point.
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

/// Recursive winding-number contribution for a cubic Bezier curve.
int WindingNumberContributionCurve(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                   const Vector2d& p3, const Vector2d& point, double tolerance,
                                   int depth = 0) {
  if (depth > kHitTestMaxDepth || IsCurveFlatEnough(p0, p1, p2, p3, tolerance)) {
    return WindingNumberContribution(p0, p3, point);
  }

  const Vector2d p01 = (p0 + p1) * 0.5;
  const Vector2d p12 = (p1 + p2) * 0.5;
  const Vector2d p23 = (p2 + p3) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  return WindingNumberContributionCurve(p0, p01, p012, p0123, point, tolerance, depth + 1) +
         WindingNumberContributionCurve(p0123, p123, p23, p3, point, tolerance, depth + 1);
}

/// Recursive check whether a point is within \p tolerance of a cubic Bezier.
bool IsPointOnCubicBezier(const Vector2d& point, const Vector2d& p0, const Vector2d& p1,
                          const Vector2d& p2, const Vector2d& p3, double tolerance,
                          int depth = 0) {
  if (depth > kHitTestMaxDepth || IsCurveFlatEnough(p0, p1, p2, p3, tolerance)) {
    return DistanceFromPointToLine(point, p0, p3) <= tolerance;
  }

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

bool Path::isInside(const Vector2d& point, FillRule fillRule) const {
  constexpr double kIsInsideTolerance = 0.1;

  int windingNumber = 0;
  Vector2d currentPoint;
  Vector2d subpathStart;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& cmd = commands_[i];
    switch (cmd.verb) {
      case Verb::MoveTo: {
        currentPoint = points_[cmd.pointIndex];
        subpathStart = currentPoint;
        break;
      }

      case Verb::LineTo: {
        const Vector2d& endPt = points_[cmd.pointIndex];
        if (DistanceFromPointToLine(point, currentPoint, endPt) <= kIsInsideTolerance) {
          return true;
        }
        windingNumber += WindingNumberContribution(currentPoint, endPt, point);
        currentPoint = endPt;
        break;
      }

      case Verb::QuadTo: {
        // Elevate to cubic for hit testing.
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& endPt = points_[cmd.pointIndex + 1];
        const Vector2d c1 = currentPoint + (control - currentPoint) * (2.0 / 3.0);
        const Vector2d c2 = endPt + (control - endPt) * (2.0 / 3.0);
        if (IsPointOnCubicBezier(point, currentPoint, c1, c2, endPt, kIsInsideTolerance)) {
          return true;
        }
        windingNumber +=
            WindingNumberContributionCurve(currentPoint, c1, c2, endPt, point, kHitTestTolerance);
        currentPoint = endPt;
        break;
      }

      case Verb::CurveTo: {
        const Vector2d& c1 = points_[cmd.pointIndex];
        const Vector2d& c2 = points_[cmd.pointIndex + 1];
        const Vector2d& endPt = points_[cmd.pointIndex + 2];
        if (IsPointOnCubicBezier(point, currentPoint, c1, c2, endPt, kIsInsideTolerance)) {
          return true;
        }
        windingNumber +=
            WindingNumberContributionCurve(currentPoint, c1, c2, endPt, point, kHitTestTolerance);
        currentPoint = endPt;
        break;
      }

      case Verb::ClosePath: {
        if (DistanceFromPointToLine(point, currentPoint, subpathStart) <= kIsInsideTolerance) {
          return true;
        }
        windingNumber += WindingNumberContribution(currentPoint, subpathStart, point);
        currentPoint = subpathStart;
        break;
      }
    }
  }

  if (fillRule == FillRule::NonZero) {
    return windingNumber != 0;
  } else if (fillRule == FillRule::EvenOdd) {
    return (windingNumber % 2) != 0;
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

bool Path::isOnPath(const Vector2d& point, double strokeWidth) const {
  Vector2d currentPoint;
  Vector2d subpathStart;

  for (const Command& cmd : commands_) {
    switch (cmd.verb) {
      case Verb::MoveTo: {
        currentPoint = points_[cmd.pointIndex];
        subpathStart = currentPoint;
        break;
      }

      case Verb::LineTo: {
        const Vector2d& endPt = points_[cmd.pointIndex];
        if (DistanceFromPointToLine(point, currentPoint, endPt) <= strokeWidth) {
          return true;
        }
        currentPoint = endPt;
        break;
      }

      case Verb::QuadTo: {
        // Elevate to cubic for hit testing.
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& endPt = points_[cmd.pointIndex + 1];
        const Vector2d c1 = currentPoint + (control - currentPoint) * (2.0 / 3.0);
        const Vector2d c2 = endPt + (control - endPt) * (2.0 / 3.0);
        if (IsPointOnCubicBezier(point, currentPoint, c1, c2, endPt, strokeWidth)) {
          return true;
        }
        currentPoint = endPt;
        break;
      }

      case Verb::CurveTo: {
        const Vector2d& c1 = points_[cmd.pointIndex];
        const Vector2d& c2 = points_[cmd.pointIndex + 1];
        const Vector2d& endPt = points_[cmd.pointIndex + 2];
        if (IsPointOnCubicBezier(point, currentPoint, c1, c2, endPt, strokeWidth)) {
          return true;
        }
        currentPoint = endPt;
        break;
      }

      case Verb::ClosePath: {
        if (DistanceFromPointToLine(point, currentPoint, subpathStart) <= strokeWidth) {
          return true;
        }
        currentPoint = subpathStart;
        break;
      }
    }
  }

  return false;
}

namespace {

/**
 * Interpolate two tangent vectors to find the orientation at a joint.
 *
 * If the tangents are not opposite, returns the normalized sum (the halfway direction).
 * If the tangents are opposite (a cusp), returns a perpendicular to prevTangent.
 */
Vector2d InterpolateTangents(const Vector2d& prevTangent, const Vector2d& nextTangent) {
  const Vector2d sum = prevTangent + nextTangent;

  if (!NearZero(sum.lengthSquared())) {
    return sum.normalize();
  } else {
    // Tangents are opposite; choose a perpendicular vector.
    return Vector2d(prevTangent.y, -prevTangent.x);
  }
}

/// Compute the tangent at the start (t=0) of command \p i.
Vector2d tangentAtStart(const std::vector<Path::Command>& commands,
                        const std::vector<Vector2d>& points, size_t i) {
  const auto& cmd = commands[i];
  const Vector2d start = startPointOfCommand(commands, points, i);

  switch (cmd.verb) {
    case Path::Verb::MoveTo:
      // Tangent of a MoveTo is the tangent at the start of the next command.
      if (i + 1 < commands.size()) {
        return tangentAtStart(commands, points, i + 1);
      }
      return Vector2d::Zero();

    case Path::Verb::LineTo:
      return points[cmd.pointIndex] - start;

    case Path::Verb::QuadTo: {
      // Derivative of quadratic at t=0: 2*(P1 - P0).
      Vector2d tangent = 2.0 * (points[cmd.pointIndex] - start);
      if (NearZero(tangent.lengthSquared())) {
        // Degenerate: control == start, use (end - start).
        tangent = points[cmd.pointIndex + 1] - start;
      }
      return tangent;
    }

    case Path::Verb::CurveTo: {
      // Derivative of cubic at t=0: 3*(P1 - P0).
      Vector2d tangent = 3.0 * (points[cmd.pointIndex] - start);
      if (NearZero(tangent.lengthSquared())) {
        // Degenerate: c1 == start, try c2.
        tangent = 3.0 * (points[cmd.pointIndex + 1] - start);
        if (NearZero(tangent.lengthSquared())) {
          tangent = points[cmd.pointIndex + 2] - start;
        }
      }
      return tangent;
    }

    case Path::Verb::ClosePath: {
      // ClosePath is a line from current point back to the subpath start.
      return endPointOfCommand(commands, points, i) - start;
    }
  }
  return Vector2d::Zero();
}

/// Compute the tangent at the end (t=1) of command \p i.
Vector2d tangentAtEnd(const std::vector<Path::Command>& commands,
                      const std::vector<Vector2d>& points, size_t i) {
  const auto& cmd = commands[i];
  const Vector2d start = startPointOfCommand(commands, points, i);

  switch (cmd.verb) {
    case Path::Verb::MoveTo:
      if (i + 1 < commands.size()) {
        return tangentAtStart(commands, points, i + 1);
      }
      return Vector2d::Zero();

    case Path::Verb::LineTo:
      return points[cmd.pointIndex] - start;

    case Path::Verb::QuadTo: {
      // Derivative of quadratic at t=1: 2*(P2 - P1).
      Vector2d tangent = 2.0 * (points[cmd.pointIndex + 1] - points[cmd.pointIndex]);
      if (NearZero(tangent.lengthSquared())) {
        tangent = points[cmd.pointIndex + 1] - start;
      }
      return tangent;
    }

    case Path::Verb::CurveTo: {
      // Derivative of cubic at t=1: 3*(P3 - P2).
      Vector2d tangent = 3.0 * (points[cmd.pointIndex + 2] - points[cmd.pointIndex + 1]);
      if (NearZero(tangent.lengthSquared())) {
        tangent = 3.0 * (points[cmd.pointIndex + 2] - points[cmd.pointIndex]);
        if (NearZero(tangent.lengthSquared())) {
          tangent = points[cmd.pointIndex + 2] - start;
        }
      }
      return tangent;
    }

    case Path::Verb::ClosePath: {
      return endPointOfCommand(commands, points, i) - start;
    }
  }
  return Vector2d::Zero();
}

/// Find the index of the ClosePath command for the subpath starting at \p moveToIndex,
/// or size_t(-1) if the subpath is open.
size_t findClosePathIndex(const std::vector<Path::Command>& commands, size_t moveToIndex) {
  for (size_t j = moveToIndex + 1; j < commands.size(); ++j) {
    if (commands[j].verb == Path::Verb::ClosePath) {
      return j;
    }
    if (commands[j].verb == Path::Verb::MoveTo) {
      return size_t(-1);  // Open subpath (next subpath started before close).
    }
  }
  return size_t(-1);  // Open subpath (end of path without close).
}

}  // namespace

std::vector<Path::Vertex> Path::vertices() const {
  std::vector<Vertex> result;
  std::optional<size_t> openPathCommand;
  size_t closePathIndex = size_t(-1);
  bool justMoved = false;

  bool wasInternal = false;
  for (size_t i = 0; i < commands_.size(); ++i) {
    const Command& command = commands_[i];

    // Skip intermediate arc decomposition segments — only the first and last segments of an arc
    // produce vertices for marker placement.
    const bool shouldSkip = wasInternal;
    wasInternal = command.isInternal;
    if (shouldSkip) {
      continue;
    }

    if (command.verb == Verb::MoveTo) {
      if (openPathCommand && !justMoved) {
        assert(i > 0);

        // Place a vertex at the end of the previous segment. For open subpaths, the orientation is
        // the direction of the line. Skip if the previous command was also a MoveTo (empty subpath).
        const Vector2d point = endPointOfCommand(commands_, points_, i - 1);
        const Vector2d orientation = tangentAtEnd(commands_, points_, i - 1).normalize();
        result.push_back(Vertex{point, orientation});
      }

      openPathCommand = i;
      closePathIndex = findClosePathIndex(commands_, i);
      justMoved = true;

    } else if (command.verb == Verb::ClosePath) {
      // If this ClosePath draws a line back to the starting point, place a vertex at the current
      // point. Since this is a closed subpath, the orientation is interpolated between the end of
      // the previous segment and the start of the ClosePath line.
      assert(openPathCommand);
      assert(i > 0);

      const Vector2d startPoint = endPointOfCommand(commands_, points_, i - 1);
      const Vector2d endPoint = endPointOfCommand(commands_, points_, i);

      // Only place a vertex if the ClosePath line has non-zero length.
      if (!NearZero((startPoint - endPoint).lengthSquared())) {
        const Vector2d prevTangent = tangentAtEnd(commands_, points_, i - 1).normalize();
        const Vector2d nextTangent = tangentAtStart(commands_, points_, i).normalize();

        const Vector2d orientationStart = InterpolateTangents(prevTangent, nextTangent);
        result.push_back(Vertex{startPoint, orientationStart});
      }

      // Place a vertex at the subpath start point. The orientation is interpolated between the
      // end of the ClosePath line and the start of the first segment after MoveTo.
      {
        const Vector2d prevTangent = tangentAtEnd(commands_, points_, i).normalize();
        const Vector2d nextTangent = tangentAtStart(commands_, points_, *openPathCommand).normalize();

        const Vector2d orientationEnd = InterpolateTangents(prevTangent, nextTangent);
        result.push_back(Vertex{endPoint, orientationEnd});
      }

      openPathCommand = std::nullopt;
      justMoved = false;
    } else {
      // LineTo, QuadTo, or CurveTo: place a vertex at the start point.
      assert(i > 0);

      const Vector2d startPoint = startPointOfCommand(commands_, points_, i);
      const Vector2d startOrientation = tangentAtStart(commands_, points_, i).normalize();

      if (justMoved) {
        // First drawing command after a MoveTo.
        if (closePathIndex != size_t(-1)) {
          // Closed subpath: interpolate between the ClosePath tangent and the first segment.
          const Vector2d closeOrientation =
              tangentAtEnd(commands_, points_, closePathIndex).normalize();
          result.push_back(
              Vertex{startPoint, InterpolateTangents(closeOrientation, startOrientation)});
        } else {
          // Open subpath: orientation is the direction of the first segment.
          result.push_back(Vertex{startPoint, startOrientation});
        }
      } else {
        // Interior vertex: orientation is interpolated between end of previous and start of this.
        const Vector2d prevOrientation = tangentAtEnd(commands_, points_, i - 1).normalize();
        result.push_back(
            Vertex{startPoint, InterpolateTangents(prevOrientation, startOrientation)});
      }

      justMoved = false;
    }
  }

  // If the last subpath was open, place the final vertex.
  if (openPathCommand && commands_.size() > 1) {
    const Vector2d point = endPointOfCommand(commands_, points_, commands_.size() - 1);
    const Vector2d orientation = tangentAtEnd(commands_, points_, commands_.size() - 1).normalize();
    result.push_back(Vertex{point, orientation});
  }

  return result;
}

namespace {

/// Helper: find the start point for a command at the given index.
/// The start point is the last point emitted by the previous command.
Vector2d findStartPoint(const std::vector<Path::Command>& commands,
                        const std::vector<Vector2d>& points, size_t cmdIndex) {
  if (cmdIndex == 0) {
    return points.empty() ? Vector2d() : points[0];
  }

  const auto& prevCmd = commands[cmdIndex - 1];
  const size_t prevPoints = Path::pointsPerVerb(prevCmd.verb);
  if (prevPoints == 0) {
    // ClosePath: scan backwards for the last MoveTo.
    for (size_t i = cmdIndex - 1; i < cmdIndex; --i) {
      if (commands[i].verb == Path::Verb::MoveTo) {
        return points[commands[i].pointIndex];
      }
    }
    return points.empty() ? Vector2d() : points[0];
  }
  return points[prevCmd.pointIndex + prevPoints - 1];
}


}  // namespace

Path Path::cubicToQuadratic(double tolerance) const {
  PathBuilder builder;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const auto& cmd = commands_[i];
    switch (cmd.verb) {
      case Verb::MoveTo:
        builder.moveTo(points_[cmd.pointIndex]);
        break;

      case Verb::LineTo:
        builder.lineTo(points_[cmd.pointIndex]);
        break;

      case Verb::QuadTo:
        builder.quadTo(points_[cmd.pointIndex], points_[cmd.pointIndex + 1]);
        break;

      case Verb::CurveTo: {
        const Vector2d start = findStartPoint(commands_, points_, i);
        const Vector2d& c1 = points_[cmd.pointIndex];
        const Vector2d& c2 = points_[cmd.pointIndex + 1];
        const Vector2d& end = points_[cmd.pointIndex + 2];

        std::vector<Vector2d> quads;
        ApproximateCubicWithQuadratics(start, c1, c2, end, tolerance, quads);

        // quads contains pairs of (control, end) for each quadratic.
        for (size_t j = 0; j + 1 < quads.size(); j += 2) {
          builder.quadTo(quads[j], quads[j + 1]);
        }
        break;
      }

      case Verb::ClosePath:
        builder.closePath();
        break;
    }
  }

  return builder.build();
}

Path Path::toMonotonic() const {
  PathBuilder builder;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const auto& cmd = commands_[i];
    switch (cmd.verb) {
      case Verb::MoveTo:
        builder.moveTo(points_[cmd.pointIndex]);
        break;

      case Verb::LineTo:
        // Lines are always monotonic.
        builder.lineTo(points_[cmd.pointIndex]);
        break;

      case Verb::QuadTo: {
        const Vector2d start = findStartPoint(commands_, points_, i);
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& end = points_[cmd.pointIndex + 1];

        auto extrema = QuadraticYExtrema(start, control, end);
        if (extrema.empty()) {
          // Already monotonic.
          builder.quadTo(control, end);
        } else {
          // Split at the extremum.
          auto [left, right] = SplitQuadratic(start, control, end, extrema[0]);
          builder.quadTo(left[1], left[2]);
          builder.quadTo(right[1], right[2]);
        }
        break;
      }

      case Verb::CurveTo: {
        const Vector2d start = findStartPoint(commands_, points_, i);
        const Vector2d& c1 = points_[cmd.pointIndex];
        const Vector2d& c2 = points_[cmd.pointIndex + 1];
        const Vector2d& end = points_[cmd.pointIndex + 2];

        auto extrema = CubicYExtrema(start, c1, c2, end);
        if (extrema.empty()) {
          // Already monotonic.
          builder.curveTo(c1, c2, end);
        } else {
          // Collect split parameters, sorted.
          // Split the curve at each extremum. We need to adjust parameters for successive splits.
          std::vector<std::array<Vector2d, 4>> segments;
          std::array<Vector2d, 4> current = {start, c1, c2, end};

          // Split at each extremum, adjusting t values for the remaining portion.
          double prevT = 0.0;
          for (double t : extrema) {
            // Remap t to the remaining portion of the curve.
            double localT = (t - prevT) / (1.0 - prevT);
            localT = std::clamp(localT, 0.0, 1.0);

            auto [left, right] = SplitCubic(current[0], current[1], current[2], current[3], localT);
            segments.push_back(left);
            current = right;
            prevT = t;
          }
          segments.push_back(current);

          for (const auto& seg : segments) {
            builder.curveTo(seg[1], seg[2], seg[3]);
          }
        }
        break;
      }

      case Verb::ClosePath:
        builder.closePath();
        break;
    }
  }

  return builder.build();
}

std::ostream& operator<<(std::ostream& os, const Path& path) {
  for (const auto& cmd : path.commands_) {
    os << cmd.verb;
    const size_t n = Path::pointsPerVerb(cmd.verb);
    for (size_t j = 0; j < n; ++j) {
      os << " " << path.points_[cmd.pointIndex + j];
    }
    os << "\n";
  }
  return os;
}

namespace {

constexpr int kMaxFlattenDepth = 10;

void flattenQuadratic(PathBuilder& builder, const Vector2d& p0, const Vector2d& p1,
                      const Vector2d& p2, double tolerance, int depth) {
  const Vector2d mid = (p0 + p2) * 0.5;
  const double dist = (p1 - mid).length();

  if (dist <= tolerance || depth >= kMaxFlattenDepth) {
    builder.lineTo(p2);
    return;
  }

  auto [left, right] = SplitQuadratic(p0, p1, p2, 0.5);
  flattenQuadratic(builder, left[0], left[1], left[2], tolerance, depth + 1);
  flattenQuadratic(builder, right[0], right[1], right[2], tolerance, depth + 1);
}

void flattenCubic(PathBuilder& builder, const Vector2d& p0, const Vector2d& p1,
                  const Vector2d& p2, const Vector2d& p3, double tolerance, int depth) {
  const Vector2d d1 = p1 - (p0 * (2.0 / 3.0) + p3 * (1.0 / 3.0));
  const Vector2d d2 = p2 - (p0 * (1.0 / 3.0) + p3 * (2.0 / 3.0));
  const double dist = std::max(d1.length(), d2.length());

  if (dist <= tolerance || depth >= kMaxFlattenDepth) {
    builder.lineTo(p3);
    return;
  }

  auto [left, right] = SplitCubic(p0, p1, p2, p3, 0.5);
  flattenCubic(builder, left[0], left[1], left[2], left[3], tolerance, depth + 1);
  flattenCubic(builder, right[0], right[1], right[2], right[3], tolerance, depth + 1);
}

}  // namespace

Path Path::flatten(double tolerance) const {
  PathBuilder builder;

  for (size_t i = 0; i < commands_.size(); ++i) {
    const auto& cmd = commands_[i];
    switch (cmd.verb) {
      case Verb::MoveTo:
        builder.moveTo(points_[cmd.pointIndex]);
        break;

      case Verb::LineTo:
        builder.lineTo(points_[cmd.pointIndex]);
        break;

      case Verb::QuadTo: {
        const Vector2d start = findStartPoint(commands_, points_, i);
        const Vector2d& control = points_[cmd.pointIndex];
        const Vector2d& end = points_[cmd.pointIndex + 1];
        flattenQuadratic(builder, start, control, end, tolerance, 0);
        break;
      }

      case Verb::CurveTo: {
        const Vector2d start = findStartPoint(commands_, points_, i);
        const Vector2d& c1 = points_[cmd.pointIndex];
        const Vector2d& c2 = points_[cmd.pointIndex + 1];
        const Vector2d& end = points_[cmd.pointIndex + 2];
        flattenCubic(builder, start, c1, c2, end, tolerance, 0);
        break;
      }

      case Verb::ClosePath:
        builder.closePath();
        break;
    }
  }

  return builder.build();
}

namespace {

/// Compute the perpendicular normal (left-hand side) of a line segment from \p a to \p b.
/// Returns a unit vector pointing to the left of the segment direction.
Vector2d segmentNormal(const Vector2d& a, const Vector2d& b) {
  const Vector2d d = b - a;
  // Rotate 90 degrees counter-clockwise: (dx, dy) -> (-dy, dx)
  return Vector2d(-d.y, d.x).normalize();
}

/// Result of computing the miter (intersection) point of two offset lines.
struct MiterResult {
  bool valid = false;       ///< True if a finite miter point exists.
  Vector2d point;           ///< The miter point (intersection of the two offset lines).
  double lengthFromVertex;  ///< Distance from the original vertex to the miter point.
};

/// Compute the intersection point of the two offset lines.
///
/// `prevNormal` and `curNormal` are the (unit) outward normals of the two adjacent
/// segments *on the current contour side* — for the inside-of-turn branch the
/// caller passes the true normals; for the outside branch the caller also passes
/// the true outward normals of the contour it's currently tracing. Both offset
/// lines are `{ x : x·n = vertex·n + halfWidth }`, and their intersection lies
/// at `vertex + miterUnit * (halfWidth / cosHalfAngle)` where
/// `cosHalfAngle = |prevNormal + curNormal| / 2`. This is the same formula used
/// by `ComputeMiter` in `strokeMiterBounds` — expressed as
/// `halfWidth / cos(halfAngleBetweenNormals)`, which equals
/// `halfWidth / sin(interiorHalfAngleAtVertex)` (the textbook SVG miter
/// formula). Importantly, this is NOT `halfWidth / sinHalfAngle`: that earlier
/// formulation happened to be correct for 90° corners (where sin=cos at 45°)
/// but drifts for any other turn — undershooting sharp inside corners of open
/// paths (the Phase 2C inverted-V symptom) and overshooting gentle outside
/// corners.
MiterResult computeMiterPoint(const Vector2d& vertex, const Vector2d& prevNormal,
                              const Vector2d& curNormal, double halfWidth) {
  MiterResult result;
  const Vector2d miterDir = prevNormal + curNormal;
  const double miterDirLen = miterDir.length();
  if (NearZero(miterDirLen, 1e-10)) {
    // 180° turn: offset lines are parallel; no finite miter.
    return result;
  }

  const Vector2d miterUnit = miterDir * (1.0 / miterDirLen);
  // cosHalfAngle = cos(half the angle between the two normals) = |n1+n2|/2.
  // Equivalently, it is sin(half the interior angle at the vertex): rotating
  // both normals 90° yields segment-direction unit vectors, which preserves
  // all relative angles.
  const double cosHalfAngle = miterDirLen * 0.5;
  if (NearZero(cosHalfAngle, 1e-10)) {
    return result;
  }

  result.valid = true;
  result.lengthFromVertex = halfWidth / cosHalfAngle;
  result.point = vertex + miterUnit * result.lengthFromVertex;
  return result;
}

/// Emit a line join between two consecutive offset segments.
///
/// \p prevEnd is the end of the previous offset segment on the current side.
/// \p curStart is the start of the current offset segment on the current side.
/// \p vertex is the original path vertex (the corner point before offset).
/// \p prevNormal and \p curNormal are the outward normals of the previous and current segments.
/// \p halfWidth is half the stroke width.
/// \p join is the line join style.
/// \p miterLimit is the SVG miter limit.
/// \p builder is the PathBuilder to emit to.
/// \p isLeftSide indicates whether we are building the left (forward) or right (backward) contour.
void emitJoin(const Vector2d& prevEnd, const Vector2d& curStart, const Vector2d& vertex,
              const Vector2d& prevNormal, const Vector2d& curNormal, double halfWidth,
              LineJoin join, double miterLimit, PathBuilder& builder, bool isLeftSide) {
  // Determine the turn direction. The cross product of the two normals tells us
  // whether the join is on the inside or outside of the turn.
  // For a left turn (counter-clockwise), the outside is on the left side.
  const double cross = prevNormal.x * curNormal.y - prevNormal.y * curNormal.x;

  // If the normals are nearly parallel, just connect with a line.
  if (NearZero(cross, 1e-10)) {
    builder.lineTo(curStart);
    return;
  }

  // Determine if this side is the outside of the turn.
  // For a left-side contour: outside when turning right (cross < 0).
  // For a right-side contour: outside when turning left (cross > 0).
  const bool isOutside = isLeftSide ? (cross < 0.0) : (cross > 0.0);

  if (!isOutside) {
    // Inside of the turn: the naive `lineTo(curStart)` connects two points
    // that are both `halfWidth` from `vertex` on different offset directions,
    // so the resulting edge zig-zags back through the interior of the stroke
    // ribbon. Neither NonZero nor EvenOdd renders that cleanly; the symptoms
    // are:
    //   - Phase 2D: full-width diagonal streaks inside stroked rects/ellipses
    //     (Geode goldens rect2, ellipse1, quadbezier1, rotated-rect).
    //   - Phase 2C: gaps / extra triangles at the concave side of sharp
    //     open-subpath corners like the inverted-V apex in the
    //     stroking_linejoin Geode golden.
    //
    // For SHARP inside corners (turn angle >= 60°, i.e.
    // `cosHalfAngle <= cos(30°) ≈ 0.866`), replace the naive edge with the
    // true intersection of the two offset lines via `computeMiterPoint`. The
    // intersection lies on both offset lines, so the polygon edge from the
    // previous segment's offset end to the miter point cancels (in winding)
    // against the backward traversal of the same offset line; the net
    // traced shape is the clean inner miter corner. This handles angular
    // polygon corners (rect 90°, inverted-V ~127°) and the Phase 2C
    // sharp-open-corner case.
    //
    // For SMOOTH flattened-curve joins (~1–10° turns), keep the original
    // `lineTo(curStart)` behavior. The correct miter point is geometrically
    // close to `curStart` but adds tiny zig-zag vertices on dense flattened
    // ellipse contours that have been observed to corrupt ray-cast winding
    // (see the `StrokeToFillClosedEllipseInteriorIsEmpty` regression). The
    // sub-halfWidth zig is visually imperceptible in that regime.
    //
    // For very-sharp inside turns beyond the miter limit (miterRatio >
    // miterLimit, close to a U-turn), fall back to the direct connection —
    // the intersection recedes toward infinity and emitting it would inject
    // spurious far-away vertices.
    constexpr double kSharpInsideCosThreshold = 0.866;  // turn angle >= 60°
    constexpr double kSharpInsideMiterRatio = 1.0 / kSharpInsideCosThreshold;
    const MiterResult miter = computeMiterPoint(vertex, prevNormal, curNormal, halfWidth);
    if (miter.valid) {
      const double miterRatio = miter.lengthFromVertex / halfWidth;
      const bool isSharp = miterRatio >= kSharpInsideMiterRatio;
      const bool withinLimit = miterRatio <= miterLimit;
      if (isSharp && withinLimit) {
        builder.lineTo(miter.point);
        return;
      }
    }
    builder.lineTo(curStart);
    return;
  }

  // Outside of the turn: apply the join style.
  switch (join) {
    case LineJoin::Bevel:
      // Simple: connect the two offset endpoints with a straight line.
      builder.lineTo(curStart);
      break;

    case LineJoin::Round: {
      // Approximate a circular arc from prevEnd to curStart around vertex.
      // We subdivide the angle into segments for a smooth approximation.
      const Vector2d fromVec = prevEnd - vertex;
      const Vector2d toVec = curStart - vertex;

      double startAngle = std::atan2(fromVec.y, fromVec.x);
      double endAngle = std::atan2(toVec.y, toVec.x);

      // Choose the shorter arc direction.
      double sweep = endAngle - startAngle;
      if (sweep > MathConstants<double>::kPi) {
        sweep -= 2.0 * MathConstants<double>::kPi;
      } else if (sweep < -MathConstants<double>::kPi) {
        sweep += 2.0 * MathConstants<double>::kPi;
      }

      // Subdivide into small arcs.
      const int numSteps = std::max(4, static_cast<int>(std::ceil(Abs(sweep) * halfWidth / 2.0)));
      for (int s = 1; s <= numSteps; ++s) {
        const double t = static_cast<double>(s) / static_cast<double>(numSteps);
        const double angle = startAngle + sweep * t;
        const Vector2d pt = vertex + Vector2d(std::cos(angle), std::sin(angle)) * halfWidth;
        builder.lineTo(pt);
      }
      break;
    }

    case LineJoin::Miter: {
      // Compute the miter point: the intersection of the two offset lines.
      // The offset lines are { x : x·n = vertex·n + halfWidth }, and their
      // intersection lies at `vertex + miterUnit * halfWidth/cosHalfAngle`,
      // where `miterUnit = (n1+n2)/|n1+n2|` and
      // `cosHalfAngle = |n1+n2|/2 = sin(interiorHalfAngle)`. This matches the
      // standard SVG miter formula `halfWidth/sin(interiorHalfAngle)` used by
      // `ComputeMiter` in `strokeMiterBounds`.
      //
      // Note: prior revisions used `halfWidth/sinHalfAngle` (where
      // sinHalfAngle is the sine of the half-angle *between the normals*),
      // which happens to coincide with the correct formula at exactly 90°
      // turns (sin 45° = cos 45°) but drifts for every other angle —
      // undershooting sharp outside miters (the inverted-V right contour
      // bug) and rejecting gentle outside miters via a spuriously-large
      // miter ratio.
      const MiterResult miter = computeMiterPoint(vertex, prevNormal, curNormal, halfWidth);
      if (!miter.valid) {
        builder.lineTo(curStart);
        break;
      }

      const double miterRatio = miter.lengthFromVertex / halfWidth;
      if (miterRatio > miterLimit) {
        builder.lineTo(curStart);
      } else {
        builder.lineTo(miter.point);
        builder.lineTo(curStart);
      }
      break;
    }
  }
}

/// Emit a line cap at a subpath endpoint.
///
/// \p point is the endpoint.
/// \p direction is the tangent direction at the endpoint (pointing outward from the subpath).
/// \p halfWidth is half the stroke width.
/// \p cap is the cap style.
/// \p builder is the PathBuilder to emit to.
void emitCap(const Vector2d& point, const Vector2d& direction, double halfWidth, LineCap cap,
             PathBuilder& builder) {
  const Vector2d normal = Vector2d(-direction.y, direction.x);
  const Vector2d leftPt = point + normal * halfWidth;
  const Vector2d rightPt = point - normal * halfWidth;

  switch (cap) {
    case LineCap::Butt:
      // Connect left to right directly (the caller has already placed us at leftPt).
      builder.lineTo(rightPt);
      break;

    case LineCap::Square: {
      // Extend by halfWidth in the direction of the tangent.
      const Vector2d extension = direction * halfWidth;
      builder.lineTo(leftPt + extension);
      builder.lineTo(rightPt + extension);
      builder.lineTo(rightPt);
      break;
    }

    case LineCap::Round: {
      // Semicircle from leftPt around to rightPt.
      const double startAngle = std::atan2(normal.y, normal.x);
      // Sweep PI radians (semicircle) in the direction from left to right around the cap.
      const double sweep = -MathConstants<double>::kPi;

      const int numSteps = std::max(8, static_cast<int>(std::ceil(halfWidth * 2.0)));
      for (int s = 1; s <= numSteps; ++s) {
        const double t = static_cast<double>(s) / static_cast<double>(numSteps);
        const double angle = startAngle + sweep * t;
        const Vector2d pt = point + Vector2d(std::cos(angle), std::sin(angle)) * halfWidth;
        builder.lineTo(pt);
      }
      break;
    }
  }
}

/// Represents a subpath extracted from a flattened path (lines only).
struct FlatSubpath {
  std::vector<Vector2d> points;
  bool closed = false;
};

/// Extract subpaths from a flattened path.
std::vector<FlatSubpath> extractSubpaths(const Path& path) {
  std::vector<FlatSubpath> subpaths;
  FlatSubpath current;
  Vector2d moveToPoint;

  for (const auto& cmd : path.commands()) {
    switch (cmd.verb) {
      case Path::Verb::MoveTo:
        if (current.points.size() >= 2) {
          subpaths.push_back(std::move(current));
        }
        current = FlatSubpath();
        moveToPoint = path.points()[cmd.pointIndex];
        current.points.push_back(moveToPoint);
        break;

      case Path::Verb::LineTo:
        current.points.push_back(path.points()[cmd.pointIndex]);
        break;

      case Path::Verb::ClosePath:
        // Close: add closing line back to moveTo if needed.
        if (!current.points.empty() &&
            current.points.back().distanceSquared(moveToPoint) > 1e-20) {
          current.points.push_back(moveToPoint);
        }
        current.closed = true;
        subpaths.push_back(std::move(current));
        current = FlatSubpath();
        current.points.push_back(moveToPoint);
        break;

      case Path::Verb::QuadTo:
      case Path::Verb::CurveTo:
        // Should not appear in flattened path; ignore.
        break;
    }
  }

  if (current.points.size() >= 2) {
    subpaths.push_back(std::move(current));
  }

  return subpaths;
}

/// Build the stroke outline for a single subpath.
void strokeSubpath(const FlatSubpath& subpath, const StrokeStyle& style, PathBuilder& builder) {
  const auto& pts = subpath.points;
  const size_t n = pts.size();
  if (n < 2) {
    return;
  }

  const double halfWidth = style.width * 0.5;

  // Compute per-segment normals (left-side normals, pointing to the left of the direction).
  const size_t numSegments = n - 1;
  std::vector<Vector2d> normals(numSegments);
  for (size_t i = 0; i < numSegments; ++i) {
    normals[i] = segmentNormal(pts[i], pts[i + 1]);
  }

  // ---- Left contour (forward walk) ----
  // Offset each segment to the left by halfWidth.

  // Start point of the left contour.
  const Vector2d leftStart = pts[0] + normals[0] * halfWidth;
  builder.moveTo(leftStart);

  // Emit the first segment's end on the left side.
  builder.lineTo(pts[1] + normals[0] * halfWidth);

  // For each subsequent segment, emit a join then the segment.
  for (size_t i = 1; i < numSegments; ++i) {
    const Vector2d prevEnd = pts[i] + normals[i - 1] * halfWidth;
    const Vector2d curStart = pts[i] + normals[i] * halfWidth;

    emitJoin(prevEnd, curStart, pts[i], normals[i - 1], normals[i], halfWidth, style.join,
             style.miterLimit, builder, /*isLeftSide=*/true);

    // Emit end of current segment on left side.
    builder.lineTo(pts[i + 1] + normals[i] * halfWidth);
  }

  if (subpath.closed) {
    // For closed subpaths, join the last segment to the first.
    const Vector2d prevEnd = pts[n - 1] + normals[numSegments - 1] * halfWidth;
    const Vector2d curStart = pts[n - 1] + normals[0] * halfWidth;

    // The last point should be equal to the first point for closed subpaths.
    emitJoin(prevEnd, curStart, pts[n - 1], normals[numSegments - 1], normals[0], halfWidth,
             style.join, style.miterLimit, builder, /*isLeftSide=*/true);

    // Now walk the right contour backward.
    // For a closed path, we close the left contour and start a new subpath for the right.
    builder.closePath();

    // ---- Right contour (inner, wound opposite direction for closed paths) ----
    // Walk backward, offset to the right (i.e., negate the normal).
    const Vector2d rightStart = pts[0] - normals[0] * halfWidth;
    builder.moveTo(rightStart);

    builder.lineTo(pts[1] - normals[0] * halfWidth);

    for (size_t i = 1; i < numSegments; ++i) {
      const Vector2d prevEnd = pts[i] - normals[i - 1] * halfWidth;
      const Vector2d curStart = pts[i] - normals[i] * halfWidth;

      // On the right side, the "outside" sense is flipped.
      emitJoin(prevEnd, curStart, pts[i], Vector2d(-normals[i - 1].x, -normals[i - 1].y),
               Vector2d(-normals[i].x, -normals[i].y), halfWidth, style.join, style.miterLimit,
               builder, /*isLeftSide=*/true);

      builder.lineTo(pts[i + 1] - normals[i] * halfWidth);
    }

    // Close the right contour with a join at the start/end point.
    {
      const Vector2d prevEnd = pts[n - 1] - normals[numSegments - 1] * halfWidth;
      const Vector2d curStart = pts[n - 1] - normals[0] * halfWidth;

      emitJoin(prevEnd, curStart, pts[n - 1],
               Vector2d(-normals[numSegments - 1].x, -normals[numSegments - 1].y),
               Vector2d(-normals[0].x, -normals[0].y), halfWidth, style.join, style.miterLimit,
               builder, /*isLeftSide=*/true);
    }

    builder.closePath();
  } else {
    // ---- Open subpath: cap at end, then right contour backward, then cap at start ----

    // End cap: going from the left side to the right side at the last point.
    {
      const Vector2d endDir = (pts[n - 1] - pts[n - 2]).normalize();
      emitCap(pts[n - 1], endDir, halfWidth, style.cap, builder);
    }

    // ---- Right contour (backward walk) ----
    // Walk segments in reverse, offset to the right (negate normal).
    // We are already at the right side of the last point after the cap.
    builder.lineTo(pts[n - 1] - normals[numSegments - 1] * halfWidth);

    for (size_t i = numSegments - 1; i > 0; --i) {
      builder.lineTo(pts[i] - normals[i] * halfWidth);

      // Join between segment i and segment i-1 on the right side (walking backward).
      // When walking backward, the "previous" is normals[i] and "current" is normals[i-1],
      // but negated since we're on the right side.
      const Vector2d prevEnd = pts[i] - normals[i] * halfWidth;
      const Vector2d curStart = pts[i] - normals[i - 1] * halfWidth;

      emitJoin(prevEnd, curStart, pts[i], Vector2d(-normals[i].x, -normals[i].y),
               Vector2d(-normals[i - 1].x, -normals[i - 1].y), halfWidth, style.join,
               style.miterLimit, builder, /*isLeftSide=*/true);
    }

    builder.lineTo(pts[0] - normals[0] * halfWidth);

    // Start cap: going from the right side to the left side at the first point.
    {
      const Vector2d startDir = (pts[0] - pts[1]).normalize();
      emitCap(pts[0], startDir, halfWidth, style.cap, builder);
    }

    builder.closePath();
  }
}

/// Compute the total arc length of a flat subpath (sum of segment lengths).
double subpathLength(const FlatSubpath& subpath) {
  const auto& pts = subpath.points;
  if (pts.size() < 2) {
    return 0.0;
  }
  double total = 0.0;
  for (size_t i = 1; i < pts.size(); ++i) {
    total += (pts[i] - pts[i - 1]).length();
  }
  return total;
}

/// Resolve the effective (repeat-doubled) dash pattern and the normalized
/// dash offset. The returned pattern's total length is strictly positive
/// (callers should check the result is non-empty). All input sanity cases
/// (negative values, all-zero values, pathLength scaling) are handled here.
///
/// \p rawDashes is the raw stroke-dasharray.
/// \p dashOffset is the raw stroke-dashoffset.
/// \p actualLength is the arc length of the subpath being dashed. Returning
///   nullopt for `actualLength <= 0` keeps callers from emitting zero-length
///   dashes.
/// \p totalPathArc is the total arc length of the ENTIRE path (sum of all
///   subpath lengths). This is the reference length used together with
///   \p pathLength to compute the spec's dash-scaling ratio; per SVG2
///   pathLength refers to the whole path, so multi-subpath strokes must
///   share one scale factor rather than rescaling per subpath.
/// \p pathLength is the SVG pathLength attribute (0 means unused).
struct ResolvedDashPattern {
  std::vector<double> lengths;  ///< Even-length sequence: on, off, on, off, ...
  double totalLength = 0.0;     ///< Sum of all entries.
  double startPhase = 0.0;      ///< Phase within [0, totalLength) to begin the walk.
};

std::optional<ResolvedDashPattern> resolveDashPattern(const std::vector<double>& rawDashes,
                                                      double dashOffset, double actualLength,
                                                      double totalPathArc, double pathLength) {
  if (rawDashes.empty() || actualLength <= 0.0) {
    return std::nullopt;
  }

  // Per SVG: any negative value or all-zero pattern disables dashing (render
  // as if stroke-dasharray: none).
  double sum = 0.0;
  for (double v : rawDashes) {
    if (v < 0.0) {
      return std::nullopt;
    }
    sum += v;
  }
  if (sum <= 0.0) {
    return std::nullopt;
  }

  // Guardrail against pathological allocations: cap the doubled pattern at a
  // reasonable size. Matches spec behavior (odd -> double) but bails out if
  // the result would be unreasonably large. Real SVG dasharrays are rarely
  // longer than ~16 entries.
  constexpr size_t kMaxDashEntries = 256;
  if (rawDashes.size() > kMaxDashEntries) {
    return std::nullopt;
  }

  ResolvedDashPattern result;
  result.lengths.reserve(rawDashes.size() * 2);
  result.lengths.assign(rawDashes.begin(), rawDashes.end());
  // Per SVG spec: if the array has an odd length, it is doubled.
  if ((result.lengths.size() & 1u) != 0u) {
    const size_t half = result.lengths.size();
    for (size_t i = 0; i < half; ++i) {
      result.lengths.push_back(result.lengths[i]);
    }
  }

  // Apply pathLength scaling. When pathLength is set, dash distances are
  // expressed relative to pathLength (the author-declared total length of
  // the ENTIRE path), so we scale by totalPathArc/pathLength. The same
  // scale factor applies to every subpath — otherwise, multi-subpath
  // strokes would get inconsistent dash sizes. Per SVG2 §12.2 ("moving
  // along a path", stroke-dasharray + pathLength).
  double effectiveOffset = dashOffset;
  if (pathLength > 0.0 && totalPathArc > 0.0) {
    const double scale = totalPathArc / pathLength;
    for (double& v : result.lengths) {
      v *= scale;
    }
    effectiveOffset *= scale;
  }

  // Recompute total after scaling.
  result.totalLength = 0.0;
  for (double v : result.lengths) {
    result.totalLength += v;
  }
  if (result.totalLength <= 0.0) {
    return std::nullopt;
  }

  // Normalize the offset into [0, totalLength). Negative offsets wrap
  // backward. std::fmod can return a negative value for negative input.
  double phase = std::fmod(effectiveOffset, result.totalLength);
  if (phase < 0.0) {
    phase += result.totalLength;
  }
  result.startPhase = phase;
  return result;
}

/// Extract a sub-polyline of a subpath in the arc-length range
/// [startDist, endDist], producing a FlatSubpath containing the polyline
/// points at the exact start/end cuts (interpolated along the original
/// segment if the cut falls mid-segment). Both distances must be within
/// [0, total arc length]. Empty result if the range is degenerate.
///
/// For closed subpaths, callers handle wrap-around by calling this twice.
FlatSubpath extractPolylineRange(const FlatSubpath& subpath, double startDist, double endDist) {
  FlatSubpath result;
  const auto& pts = subpath.points;
  if (pts.size() < 2 || endDist <= startDist) {
    return result;
  }

  // Walk segments accumulating arc length.
  double cursor = 0.0;
  bool started = false;
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const Vector2d& a = pts[i];
    const Vector2d& b = pts[i + 1];
    const double segLen = (b - a).length();
    if (segLen <= 0.0) {
      continue;
    }
    const double segEnd = cursor + segLen;

    // If this entire segment is before startDist, skip.
    if (segEnd <= startDist) {
      cursor = segEnd;
      continue;
    }
    // If this entire segment is past endDist, we're done.
    if (cursor >= endDist) {
      break;
    }

    // The segment overlaps [startDist, endDist]. Compute the clipped portion.
    const double tStart = std::max(0.0, (startDist - cursor) / segLen);
    const double tEnd = std::min(1.0, (endDist - cursor) / segLen);

    if (!started) {
      const Vector2d startPt = a + (b - a) * tStart;
      result.points.push_back(startPt);
      started = true;
    }

    if (tEnd >= 1.0) {
      // Segment's full remainder is included; emit b (or skip if equal to
      // the previous point due to floating-point slop).
      if (result.points.empty() || result.points.back().distanceSquared(b) > 1e-24) {
        result.points.push_back(b);
      }
    } else {
      // End cut falls inside this segment; emit the interpolated endpoint.
      const Vector2d endPt = a + (b - a) * tEnd;
      if (result.points.empty() || result.points.back().distanceSquared(endPt) > 1e-24) {
        result.points.push_back(endPt);
      }
    }

    cursor = segEnd;
  }

  return result;
}

/// Dash a single subpath and emit its stroked dashes to the builder. Returns
/// true if any dashes were emitted, false if the pattern was invalid or the
/// subpath was too degenerate to dash.
bool strokeDashedSubpath(const FlatSubpath& subpath, const StrokeStyle& style,
                         double totalPathArc, PathBuilder& builder) {
  const double totalArc = subpathLength(subpath);
  auto maybePattern = resolveDashPattern(style.dashArray, style.dashOffset, totalArc,
                                         totalPathArc, style.pathLength);
  if (!maybePattern.has_value()) {
    return false;
  }
  const ResolvedDashPattern& pattern = *maybePattern;

  // Compute the list of (onStart, onEnd) arc-length ranges covered by the
  // dashed pattern over the subpath. Each range becomes its own open-subpath
  // stroke (with its own pair of caps).
  //
  // Walk pattern indices starting at the one that covers the startPhase.
  // For closed subpaths, we start walking at phase=startPhase but cover
  // distances [0, totalArc]; an "on" dash that straddles the end of a closed
  // path wraps to the beginning. For open subpaths, dashes that run past
  // the end are simply clipped.

  // Find the pattern index and offset-within-index at which to begin.
  // `patternCursor` is the distance along the repeating pattern where
  // distance=0 on the subpath sits. A positive dashOffset shifts the
  // pattern forward relative to the path, i.e., the path position 0 sits
  // at pattern-position `startPhase`.
  const size_t numEntries = pattern.lengths.size();
  double patternPos = pattern.startPhase;  // in [0, totalLength)
  size_t idx = 0;
  {
    double acc = 0.0;
    for (size_t i = 0; i < numEntries; ++i) {
      if (patternPos < acc + pattern.lengths[i]) {
        idx = i;
        break;
      }
      acc += pattern.lengths[i];
    }
    // Fractional offset within the current entry:
    double consumed = 0.0;
    for (size_t i = 0; i < idx; ++i) {
      consumed += pattern.lengths[i];
    }
    // `remainingInEntry` = length left in this entry from the starting path
    // position. We use this to "resume" mid-entry.
    double remainingInEntry = pattern.lengths[idx] - (patternPos - consumed);

    // Safety cap on dash segment count (pathological dash arrays on very
    // long paths). A single subpath emitting more than this many dashes is
    // almost certainly a bug or an attacker-crafted input; bail out rather
    // than hang.
    constexpr size_t kMaxDashes = 65536;
    size_t dashesEmitted = 0;

    // Guard against zero-length entries in the pattern causing an infinite
    // loop: if a full cycle of the pattern advances cursor by less than this
    // epsilon, bail. pattern.totalLength > 0 is guaranteed by the resolver,
    // so a full cycle must advance at least totalLength.
    size_t iterationsWithoutProgress = 0;
    const size_t kMaxStalledIters = numEntries + 2;  // Full cycle plus slack.

    // Walk the pattern forward over [0, totalArc]. For closed subpaths, if
    // the final "on" dash would wrap across the start, we emit it as two
    // pieces (the tail at end-of-path and the head at start-of-path).
    double cursor = 0.0;
    while (cursor < totalArc) {
      const double entryLen = (remainingInEntry > 0.0) ? remainingInEntry : pattern.lengths[idx];
      if (entryLen <= 0.0) {
        if (++iterationsWithoutProgress >= kMaxStalledIters) {
          break;
        }
        remainingInEntry = 0.0;
        idx = (idx + 1u) % numEntries;
        continue;
      }
      iterationsWithoutProgress = 0;
      const double next = cursor + entryLen;
      const bool isOn = (idx % 2u == 0u);

      if (isOn && entryLen > 0.0) {
        if (++dashesEmitted > kMaxDashes) {
          return true;  // Truncate; caller treats us as having emitted dashes.
        }
        if (subpath.closed && next > totalArc && cursor < totalArc) {
          // Wrap-around dash: tail of current cycle + head of next.
          FlatSubpath tail = extractPolylineRange(subpath, cursor, totalArc);
          const double headEnd = std::min(next - totalArc, totalArc);
          FlatSubpath head = extractPolylineRange(subpath, 0.0, headEnd);
          // Stitch tail + head together into one open-subpath polyline, so a
          // single ribbon with caps bridges the seam correctly. The last
          // point of tail should equal the first point of head if the
          // subpath's end equals its start (which it does for a proper
          // closed subpath), so dedupe to avoid a zero-length segment.
          FlatSubpath combined;
          combined.closed = false;
          combined.points = std::move(tail.points);
          for (size_t k = 0; k < head.points.size(); ++k) {
            if (combined.points.empty() ||
                combined.points.back().distanceSquared(head.points[k]) > 1e-24) {
              combined.points.push_back(head.points[k]);
            }
          }
          if (combined.points.size() >= 2) {
            strokeSubpath(combined, style, builder);
          }
        } else {
          const double dashEnd = std::min(next, totalArc);
          FlatSubpath dash = extractPolylineRange(subpath, cursor, dashEnd);
          if (dash.points.size() >= 2) {
            strokeSubpath(dash, style, builder);
          }
        }
      }

      cursor = next;
      remainingInEntry = 0.0;  // Next iteration uses full entry length.
      idx = (idx + 1u) % numEntries;
    }
  }

  return true;
}

}  // namespace

Path Path::strokeToFill(const StrokeStyle& style, double flattenTolerance) const {
  if (commands_.empty() || style.width <= 0.0) {
    return Path();
  }

  // Step 1: Flatten curves to line segments.
  const Path flattened = flatten(flattenTolerance);

  // Step 2: Extract subpaths.
  const std::vector<FlatSubpath> subpaths = extractSubpaths(flattened);

  // Step 3: Build the stroke outline for each subpath. When a dash pattern
  // is set, each subpath is split into on-dash sub-polylines (each treated
  // as an open subpath with its own caps) before offsetting.
  //
  // We compute the TOTAL arc length (sum across all subpaths) up front and
  // pass it into the per-subpath dasher. This is the reference length for
  // the SVG `pathLength` attribute — pathLength describes the entire
  // `<path>`'s length, so the scaling ratio must be consistent for every
  // subpath. Computing it per-subpath would give different-sized dashes on
  // different subpaths of the same stroke.
  PathBuilder builder;
  const bool hasDashes = !style.dashArray.empty();
  double totalPathArc = 0.0;
  if (hasDashes && style.pathLength > 0.0) {
    for (const auto& subpath : subpaths) {
      totalPathArc += subpathLength(subpath);
    }
  }
  for (const auto& subpath : subpaths) {
    if (hasDashes) {
      if (strokeDashedSubpath(subpath, style, totalPathArc, builder)) {
        continue;
      }
      // Fallback: dash pattern was degenerate (all-zero, etc.) — SVG says
      // render as solid stroke.
    }
    strokeSubpath(subpath, style, builder);
  }

  return builder.build();
}

// ============================================================================
// PathBuilder
// ============================================================================

PathBuilder& PathBuilder::moveTo(const Vector2d& point) {
  moveToPointIndex_ = static_cast<uint32_t>(path_.points_.size());
  path_.commands_.push_back({Path::Verb::MoveTo, moveToPointIndex_});
  path_.points_.push_back(point);
  lastMoveTo_ = point;
  hasMoveTo_ = true;
  return *this;
}

PathBuilder& PathBuilder::lineTo(const Vector2d& point) {
  ensureMoveTo();
  path_.commands_.push_back({Path::Verb::LineTo, static_cast<uint32_t>(path_.points_.size())});
  path_.points_.push_back(point);
  return *this;
}

PathBuilder& PathBuilder::quadTo(const Vector2d& control, const Vector2d& end) {
  ensureMoveTo();
  path_.commands_.push_back({Path::Verb::QuadTo, static_cast<uint32_t>(path_.points_.size())});
  path_.points_.push_back(control);
  path_.points_.push_back(end);
  return *this;
}

PathBuilder& PathBuilder::curveTo(const Vector2d& c1, const Vector2d& c2, const Vector2d& end) {
  ensureMoveTo();
  path_.commands_.push_back({Path::Verb::CurveTo, static_cast<uint32_t>(path_.points_.size())});
  path_.points_.push_back(c1);
  path_.points_.push_back(c2);
  path_.points_.push_back(end);
  return *this;
}

PathBuilder& PathBuilder::arcTo(const Vector2d& radius, double rotationRadians, bool largeArc,
                                bool sweep, const Vector2d& end) {
  ensureMoveTo();
  const Vector2d start = currentPoint();

  // SVG arc decomposition per Appendix F.6:
  // https://www.w3.org/TR/SVG/implnote.html#ArcImplementationNotes

  constexpr double kDistanceSqEpsilon = 1e-14;

  if (NearZero(start.distanceSquared(end), kDistanceSqEpsilon)) {
    return *this;  // No-op: end point equals start.
  }

  if (NearZero(radius.x) || NearZero(radius.y)) {
    lineTo(end);  // Zero radius: fallback to line segment.
    return *this;
  }

  const double sinRot = std::sin(rotationRadians);
  const double cosRot = std::cos(rotationRadians);

  // Rotate extent to find major axis.
  const Vector2d extent = (start - end) * 0.5;
  const Vector2d majorAxis = extent.rotate(cosRot, -sinRot);

  // B.2.5 Correct out-of-range radii.
  const Vector2d absR(Abs(radius.x), Abs(radius.y));
  const double lambda = (majorAxis.x * majorAxis.x) / (absR.x * absR.x) +
                        (majorAxis.y * majorAxis.y) / (absR.y * absR.y);
  const Vector2d r = (lambda > 1.0) ? absR * std::sqrt(lambda) : absR;

  // Eq 5.2: Ellipse center.
  double k = r.x * r.x * majorAxis.y * majorAxis.y + r.y * r.y * majorAxis.x * majorAxis.x;
  if (NearZero(k)) {
    lineTo(end);
    return *this;
  }
  k = std::sqrt(Abs((r.x * r.x * r.y * r.y) / k - 1.0));
  if (sweep == largeArc) {
    k = -k;
  }
  const Vector2d centerNoRot(k * r.x * majorAxis.y / r.y, -k * r.y * majorAxis.x / r.x);
  const Vector2d center = centerNoRot.rotate(cosRot, sinRot) + (start + end) * 0.5;

  // Compute start angle and delta.
  const Vector2d intStart = (majorAxis - centerNoRot) / r;
  const Vector2d intEnd = (-majorAxis - centerNoRot) / r;

  double intStartLen = intStart.length();
  if (NearZero(intStartLen)) {
    lineTo(end);
    return *this;
  }
  const double theta =
      std::acos(Clamp(intStart.x / intStartLen, -1.0, 1.0)) * (intStart.y < 0.0 ? -1.0 : 1.0);

  double crossLen = std::sqrt(intStart.lengthSquared() * intEnd.lengthSquared());
  if (NearZero(crossLen)) {
    lineTo(end);
    return *this;
  }

  double deltaTheta = std::acos(Clamp(intStart.dot(intEnd) / crossLen, -1.0, 1.0));
  if (intStart.x * intEnd.y - intEnd.x * intStart.y < 0.0) {
    deltaTheta = -deltaTheta;
  }
  if (sweep && deltaTheta < 0.0) {
    deltaTheta += MathConstants<double>::kPi * 2.0;
  } else if (!sweep && deltaTheta > 0.0) {
    deltaTheta -= MathConstants<double>::kPi * 2.0;
  }

  // Emit cubic Bézier segments.
  const size_t numSegs =
      static_cast<size_t>(std::ceil(Abs(deltaTheta / (MathConstants<double>::kPi * 0.5 + 0.001))));
  const Vector2d dir(cosRot, sinRot);
  const double thetaInc = deltaTheta / static_cast<double>(numSegs);

  for (size_t i = 0; i < numSegs; ++i) {
    const double ts = theta + static_cast<double>(i) * thetaInc;
    const double te = theta + static_cast<double>(i + 1) * thetaInc;
    const double halfTheta = 0.5 * (te - ts);
    const double sinHalfHalf = std::sin(halfTheta * 0.5);
    const double t = (8.0 / 3.0) * sinHalfHalf * sinHalfHalf / std::sin(halfTheta);

    const double cosTs = std::cos(ts);
    const double sinTs = std::sin(ts);
    const Vector2d p0 = r * Vector2d(cosTs - t * sinTs, sinTs + t * cosTs);

    const double cosTe = std::cos(te);
    const double sinTe = std::sin(te);
    const Vector2d p2 = r * Vector2d(cosTe, sinTe);
    const Vector2d p1 = p2 + r * Vector2d(t * sinTe, -t * cosTe);

    curveTo(center + p0.rotate(dir.x, dir.y), center + p1.rotate(dir.x, dir.y),
            center + p2.rotate(dir.x, dir.y));
    // Mark all intermediate arc segments so vertices() skips them for marker placement.
    if (i < numSegs - 1) {
      path_.commands_.back().isInternal = true;
    }
  }

  return *this;
}

PathBuilder& PathBuilder::closePath() {
  if (!hasMoveTo_) {
    return *this;  // No open subpath — consecutive closePaths are no-ops.
  }
  path_.commands_.push_back({Path::Verb::ClosePath, moveToPointIndex_});
  hasMoveTo_ = false;
  return *this;
}

PathBuilder& PathBuilder::addRect(const Box2d& rect) {
  moveTo(rect.topLeft);
  lineTo(Vector2d(rect.bottomRight.x, rect.topLeft.y));
  lineTo(rect.bottomRight);
  lineTo(Vector2d(rect.topLeft.x, rect.bottomRight.y));
  closePath();
  return *this;
}

PathBuilder& PathBuilder::addRoundedRect(const Box2d& rect, double rx, double ry) {
  // Clamp radii.
  rx = std::min(rx, rect.width() * 0.5);
  ry = std::min(ry, rect.height() * 0.5);

  if (rx <= 0.0 || ry <= 0.0) {
    return addRect(rect);
  }

  // Cubic Bézier approximation of quarter-circle: control point offset = radius * kappa.
  constexpr double kKappa = 0.5522847498;
  const double kx = rx * kKappa;
  const double ky = ry * kKappa;

  const double x0 = rect.topLeft.x;
  const double y0 = rect.topLeft.y;
  const double x1 = rect.bottomRight.x;
  const double y1 = rect.bottomRight.y;

  moveTo(Vector2d(x0 + rx, y0));
  lineTo(Vector2d(x1 - rx, y0));
  curveTo(Vector2d(x1 - rx + kx, y0), Vector2d(x1, y0 + ry - ky), Vector2d(x1, y0 + ry));
  lineTo(Vector2d(x1, y1 - ry));
  curveTo(Vector2d(x1, y1 - ry + ky), Vector2d(x1 - rx + kx, y1), Vector2d(x1 - rx, y1));
  lineTo(Vector2d(x0 + rx, y1));
  curveTo(Vector2d(x0 + rx - kx, y1), Vector2d(x0, y1 - ry + ky), Vector2d(x0, y1 - ry));
  lineTo(Vector2d(x0, y0 + ry));
  curveTo(Vector2d(x0, y0 + ry - ky), Vector2d(x0 + rx - kx, y0), Vector2d(x0 + rx, y0));
  closePath();
  return *this;
}

PathBuilder& PathBuilder::addEllipse(const Box2d& bounds) {
  const Vector2d center = (bounds.topLeft + bounds.bottomRight) * 0.5;
  const Vector2d r = bounds.size() * 0.5;

  constexpr double kKappa = 0.5522847498;
  const double kx = r.x * kKappa;
  const double ky = r.y * kKappa;

  moveTo(Vector2d(center.x + r.x, center.y));
  curveTo(Vector2d(center.x + r.x, center.y + ky), Vector2d(center.x + kx, center.y + r.y),
          Vector2d(center.x, center.y + r.y));
  curveTo(Vector2d(center.x - kx, center.y + r.y), Vector2d(center.x - r.x, center.y + ky),
          Vector2d(center.x - r.x, center.y));
  curveTo(Vector2d(center.x - r.x, center.y - ky), Vector2d(center.x - kx, center.y - r.y),
          Vector2d(center.x, center.y - r.y));
  curveTo(Vector2d(center.x + kx, center.y - r.y), Vector2d(center.x + r.x, center.y - ky),
          Vector2d(center.x + r.x, center.y));
  closePath();
  return *this;
}

PathBuilder& PathBuilder::addCircle(const Vector2d& center, double radius) {
  return addEllipse(Box2d(center - Vector2d(radius, radius), center + Vector2d(radius, radius)));
}

PathBuilder& PathBuilder::addPath(const Path& path) {
  for (const auto& cmd : path.commands_) {
    switch (cmd.verb) {
      case Path::Verb::MoveTo:
        moveTo(path.points_[cmd.pointIndex]);
        break;
      case Path::Verb::LineTo:
        lineTo(path.points_[cmd.pointIndex]);
        break;
      case Path::Verb::QuadTo:
        quadTo(path.points_[cmd.pointIndex], path.points_[cmd.pointIndex + 1]);
        break;
      case Path::Verb::CurveTo:
        curveTo(path.points_[cmd.pointIndex], path.points_[cmd.pointIndex + 1],
                path.points_[cmd.pointIndex + 2]);
        break;
      case Path::Verb::ClosePath:
        closePath();
        break;
    }
  }
  return *this;
}

Vector2d PathBuilder::currentPoint() const {
  if (path_.points_.empty()) {
    return Vector2d();
  }
  return path_.points_.back();
}

Path PathBuilder::build() {
  Path result = std::move(path_);
  path_ = Path();
  lastMoveTo_ = Vector2d();
  moveToPointIndex_ = 0;
  hasMoveTo_ = false;
  return result;
}

void PathBuilder::ensureMoveTo() {
  if (!hasMoveTo_) {
    moveTo(lastMoveTo_);
  }
}

}  // namespace donner
