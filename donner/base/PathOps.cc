#include "donner/base/PathOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/BezierUtils.h"
#include "donner/base/Box.h"
#include "donner/base/MathUtils.h"

namespace donner {
namespace {

enum class SegmentKind : std::uint8_t {
  Line,
  Quad,
  Cubic,
};

struct SegmentSplit {
  double t = 0.0;
  std::optional<Vector2d> point;
};

struct Segment {
  SegmentKind kind = SegmentKind::Line;
  Vector2d p0;
  Vector2d p1;
  Vector2d p2;
  Vector2d p3;
  std::size_t inputIndex = 0;
  std::size_t contourIndex = 0;
  std::vector<SegmentSplit> splits = {{0.0, std::nullopt}, {1.0, std::nullopt}};
};

struct InputPath {
  Path path;
  FillRule fillRule = FillRule::NonZero;
};

struct Intersection {
  double t0 = 0.0;
  double t1 = 0.0;
};

struct SegmentIntersectionResult {
  std::vector<Intersection> intersections;
  bool tooComplex = false;
};

struct Edge {
  SegmentKind kind = SegmentKind::Line;
  Vector2d p0;
  Vector2d p1;
  Vector2d p2;
  Vector2d p3;
};

struct PointKey {
  std::int64_t x = 0;
  std::int64_t y = 0;

  bool operator==(const PointKey& other) const = default;
  bool operator<(const PointKey& other) const {
    if (x != other.x) {
      return x < other.x;
    }
    return y < other.y;
  }
};

struct TraceEdge {
  Edge edge;
  PointKey startKey;
  PointKey endKey;
  bool used = false;
};

struct CubicSlice {
  std::array<Vector2d, 4> points;
  double t0 = 0.0;
  double t1 = 1.0;
};

struct IntersectionSearchBudget {
  std::size_t maxSteps = 0;
  std::size_t steps = 0;
};

struct CubicIntersectionSearch {
  std::vector<Intersection> intersections;
  IntersectionSearchBudget* budget = nullptr;
  std::size_t maxIntersections = 0;
};

std::array<Vector2d, 3> SubQuadratic(const Segment& segment, double t0, double t1);
std::array<Vector2d, 4> SubCubic(const Segment& segment, double t0, double t1);

bool IsFinite(const Vector2d& point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

double NormalizedTolerance(const PathBooleanOptions& options) {
  if (!std::isfinite(options.geometricTolerance) || options.geometricTolerance <= 0.0) {
    return 1e-6;
  }
  return options.geometricTolerance;
}

Vector2d Lerp(const Vector2d& a, const Vector2d& b, double t) {
  return a + (b - a) * t;
}

bool NearPoint(const Vector2d& a, const Vector2d& b, double tolerance) {
  return a.distanceSquared(b) <= tolerance * tolerance;
}

double DistanceFromPointToLine(const Vector2d& point, const Vector2d& a, const Vector2d& b) {
  const Vector2d ab = b - a;
  const Vector2d ap = point - a;
  const double abLengthSquared = ab.lengthSquared();
  if (NearZero(abLengthSquared)) {
    return ap.length();
  }
  const double t = Clamp(ap.dot(ab) / abLengthSquared, 0.0, 1.0);
  return (point - (a + t * ab)).length();
}

bool IsCurveFlatEnough(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                       const Vector2d& p3, double tolerance) {
  return DistanceFromPointToLine(p1, p0, p3) <= tolerance &&
         DistanceFromPointToLine(p2, p0, p3) <= tolerance;
}

int WindingNumberContribution(const Vector2d& p0, const Vector2d& p1, const Vector2d& point) {
  if (p0.y <= point.y) {
    if (p1.y > point.y && ((p1 - p0).cross(point - p0)) > 0.0) {
      return 1;
    }
  } else {
    if (p1.y <= point.y && ((p1 - p0).cross(point - p0)) < 0.0) {
      return -1;
    }
  }
  return 0;
}

int WindingNumberContributionCurve(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                   const Vector2d& p3, const Vector2d& point, double tolerance,
                                   int depth = 0) {
  // A cubic Bezier crosses a horizontal ray at most three times, so a modestly
  // subdivided polyline already isolates every winding crossing. IsCurveFlatEnough
  // uses an absolute tolerance, so near-degenerate cubics never flatten and always
  // recurse to the cap; keeping the cap low bounds the per-curve work (2^depth leaf
  // evaluations) and prevents adversarial inputs from exhausting the fuzzer timeout
  // while StrictPathContains classifies many edges.
  constexpr int kMaxDepth = 10;
  if (depth >= kMaxDepth || IsCurveFlatEnough(p0, p1, p2, p3, tolerance)) {
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

bool StrictPathContains(const Path& path, const Vector2d& point, FillRule fillRule,
                        double tolerance) {
  int windingNumber = 0;
  Vector2d currentPoint;
  Vector2d subpathStart;

  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo:
        currentPoint = points[0];
        subpathStart = points[0];
        break;
      case Path::Verb::LineTo:
        windingNumber += WindingNumberContribution(currentPoint, points[0], point);
        currentPoint = points[0];
        break;
      case Path::Verb::QuadTo: {
        const Vector2d c1 = currentPoint + (points[0] - currentPoint) * (2.0 / 3.0);
        const Vector2d c2 = points[1] + (points[0] - points[1]) * (2.0 / 3.0);
        windingNumber +=
            WindingNumberContributionCurve(currentPoint, c1, c2, points[1], point, tolerance);
        currentPoint = points[1];
        break;
      }
      case Path::Verb::CurveTo:
        windingNumber += WindingNumberContributionCurve(currentPoint, points[0], points[1],
                                                        points[2], point, tolerance);
        currentPoint = points[2];
        break;
      case Path::Verb::ClosePath:
        windingNumber += WindingNumberContribution(currentPoint, subpathStart, point);
        currentPoint = subpathStart;
        break;
    }
  });

  switch (fillRule) {
    case FillRule::NonZero: return windingNumber != 0;
    case FillRule::EvenOdd: return (windingNumber % 2) != 0;
  }
  return false;
}

bool PathsNearEqual(const Path& lhs, const Path& rhs, double tolerance) {
  if (lhs.commands().size() != rhs.commands().size() ||
      lhs.points().size() != rhs.points().size()) {
    return false;
  }

  for (std::size_t i = 0; i < lhs.commands().size(); ++i) {
    const Path::Command& lhsCommand = lhs.commands()[i];
    const Path::Command& rhsCommand = rhs.commands()[i];
    if (lhsCommand.verb != rhsCommand.verb || lhsCommand.pointIndex != rhsCommand.pointIndex ||
        lhsCommand.isInternal != rhsCommand.isInternal) {
      return false;
    }
  }

  for (std::size_t i = 0; i < lhs.points().size(); ++i) {
    if (!NearPoint(lhs.points()[i], rhs.points()[i], tolerance)) {
      return false;
    }
  }

  return true;
}

PathBooleanResult ResultWithSinglePath(Path path, const PathBooleanOptions& options) {
  if (path.verbCount() > options.maxOutputCommands) {
    return {
        .status = PathBooleanStatus::TooComplex,
        .diagnostics = {"Path boolean output exceeded maxOutputCommands"},
    };
  }
  if (path.empty()) {
    return {
        .status = PathBooleanStatus::EmptyResult,
        .diagnostics = {"Path boolean is empty after duplicate input reduction"},
    };
  }
  return {.status = PathBooleanStatus::Ok, .paths = {std::move(path)}};
}

std::optional<PathBooleanResult> TryDuplicateInputShortcut(
    PathBooleanOp op, std::span<const InputPath> transformedInputs,
    const PathBooleanOptions& options, double tolerance) {
  if (transformedInputs.size() < 2u) {
    return std::nullopt;
  }

  const InputPath& first = transformedInputs.front();
  for (std::size_t i = 1; i < transformedInputs.size(); ++i) {
    if (transformedInputs[i].fillRule != first.fillRule ||
        !PathsNearEqual(transformedInputs[i].path, first.path, tolerance)) {
      return std::nullopt;
    }
  }

  switch (op) {
    case PathBooleanOp::Union:
    case PathBooleanOp::Intersect: return ResultWithSinglePath(first.path, options);
    case PathBooleanOp::Difference:
      return PathBooleanResult{
          .status = PathBooleanStatus::EmptyResult,
          .diagnostics = {"Path boolean is empty after duplicate input reduction"},
      };
    case PathBooleanOp::Xor:
      if ((transformedInputs.size() % 2u) == 0u) {
        return PathBooleanResult{
            .status = PathBooleanStatus::EmptyResult,
            .diagnostics = {"Path boolean is empty after duplicate input reduction"},
        };
      }
      return ResultWithSinglePath(first.path, options);
  }
  return std::nullopt;
}

Box2d SegmentBounds(const Segment& segment) {
  switch (segment.kind) {
    case SegmentKind::Line: {
      Box2d box = Box2d::CreateEmpty(segment.p0);
      box.addPoint(segment.p1);
      return box;
    }
    case SegmentKind::Quad: return QuadraticBounds(segment.p0, segment.p1, segment.p2);
    case SegmentKind::Cubic: return CubicBounds(segment.p0, segment.p1, segment.p2, segment.p3);
  }
  return Box2d();
}

Box2d CubicBoundsOf(const std::array<Vector2d, 4>& points) {
  return CubicBounds(points[0], points[1], points[2], points[3]);
}

Vector2d EvalCubicDerivative(const std::array<Vector2d, 4>& points, double t) {
  const double s = 1.0 - t;
  return 3.0 * (s * s * (points[1] - points[0]) + 2.0 * s * t * (points[2] - points[1]) +
                t * t * (points[3] - points[2]));
}

bool BoxesOverlap(const Box2d& lhs, const Box2d& rhs, double tolerance) {
  return lhs.topLeft.x <= rhs.bottomRight.x + tolerance &&
         lhs.bottomRight.x + tolerance >= rhs.topLeft.x &&
         lhs.topLeft.y <= rhs.bottomRight.y + tolerance &&
         lhs.bottomRight.y + tolerance >= rhs.topLeft.y;
}

double BoxMaxExtent(const Box2d& box) {
  return std::max(std::abs(box.width()), std::abs(box.height()));
}

std::size_t SaturatingMultiply(std::size_t lhs, std::size_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return std::numeric_limits<std::size_t>::max();
  }
  return lhs * rhs;
}

std::size_t MaxIntersectionSearchSteps(const PathBooleanOptions& options,
                                       std::size_t segmentCount) {
  constexpr std::size_t kMinIntersectionSearchSteps = 512;
  constexpr std::size_t kSearchStepsPerSegment = 64;
  constexpr std::size_t kSearchStepsPerAllowedIntersection = 1;

  const std::size_t segmentBudget = std::max(
      kMinIntersectionSearchSteps, SaturatingMultiply(segmentCount, kSearchStepsPerSegment));
  const std::size_t intersectionBudget =
      SaturatingMultiply(options.maxIntersections, kSearchStepsPerAllowedIntersection);

  return std::max(segmentBudget, intersectionBudget);
}

std::array<Vector2d, 4> SegmentAsCubic(const Segment& segment) {
  switch (segment.kind) {
    case SegmentKind::Line:
      return {segment.p0, Lerp(segment.p0, segment.p1, 1.0 / 3.0),
              Lerp(segment.p0, segment.p1, 2.0 / 3.0), segment.p1};
    case SegmentKind::Quad: {
      const Vector2d c1 = segment.p0 + (segment.p1 - segment.p0) * (2.0 / 3.0);
      const Vector2d c2 = segment.p2 + (segment.p1 - segment.p2) * (2.0 / 3.0);
      return {segment.p0, c1, c2, segment.p2};
    }
    case SegmentKind::Cubic: return {segment.p0, segment.p1, segment.p2, segment.p3};
  }
  return {};
}

Vector2d SegmentPointAt(const Segment& segment, double t) {
  switch (segment.kind) {
    case SegmentKind::Line: return Lerp(segment.p0, segment.p1, t);
    case SegmentKind::Quad: return EvalQuadratic(segment.p0, segment.p1, segment.p2, t);
    case SegmentKind::Cubic: return EvalCubic(segment.p0, segment.p1, segment.p2, segment.p3, t);
  }
  return {};
}

Vector2d EdgePointAt(const Edge& edge, double t) {
  switch (edge.kind) {
    case SegmentKind::Line: return Lerp(edge.p0, edge.p1, t);
    case SegmentKind::Quad: return EvalQuadratic(edge.p0, edge.p1, edge.p2, t);
    case SegmentKind::Cubic: return EvalCubic(edge.p0, edge.p1, edge.p2, edge.p3, t);
  }
  return {};
}

Vector2d EdgeTangentAt(const Edge& edge, double t) {
  switch (edge.kind) {
    case SegmentKind::Line: return edge.p1 - edge.p0;
    case SegmentKind::Quad:
      return 2.0 * ((1.0 - t) * (edge.p1 - edge.p0) + t * (edge.p2 - edge.p1));
    case SegmentKind::Cubic:
      return 3.0 * ((1.0 - t) * (1.0 - t) * (edge.p1 - edge.p0) +
                    2.0 * (1.0 - t) * t * (edge.p2 - edge.p1) + t * t * (edge.p3 - edge.p2));
  }
  return {};
}

double ClampUnit(double value) {
  return Clamp(value, 0.0, 1.0);
}

void AddSplit(Segment* segment, double t, std::optional<Vector2d> snapPoint, double tolerance) {
  const double clamped = ClampUnit(t);
  for (SegmentSplit& existing : segment->splits) {
    if (std::abs(existing.t - clamped) <= tolerance) {
      if (snapPoint.has_value()) {
        if (existing.point.has_value()) {
          existing.point = (*existing.point + *snapPoint) * 0.5;
        } else {
          existing.point = *snapPoint;
        }
      }
      return;
    }
  }
  segment->splits.push_back(SegmentSplit{.t = clamped, .point = snapPoint});
}

void AddIntersection(std::vector<Intersection>* intersections, double t0, double t1,
                     double tolerance) {
  const double clampedT0 = ClampUnit(t0);
  const double clampedT1 = ClampUnit(t1);
  for (const Intersection& existing : *intersections) {
    if (std::abs(existing.t0 - clampedT0) <= tolerance &&
        std::abs(existing.t1 - clampedT1) <= tolerance) {
      return;
    }
  }
  intersections->push_back({.t0 = clampedT0, .t1 = clampedT1});
}

bool AddBoundedIntersection(CubicIntersectionSearch* search, double t0, double t1,
                            double tolerance) {
  AddIntersection(&search->intersections, t0, t1, tolerance);
  return search->intersections.size() <= search->maxIntersections;
}

bool SegmentsNearEqualSameDirection(const Segment& lhs, const Segment& rhs, double tolerance) {
  if (lhs.kind != rhs.kind || !NearPoint(lhs.p0, rhs.p0, tolerance) ||
      !NearPoint(lhs.p1, rhs.p1, tolerance)) {
    return false;
  }

  switch (lhs.kind) {
    case SegmentKind::Line: return true;
    case SegmentKind::Quad: return NearPoint(lhs.p2, rhs.p2, tolerance);
    case SegmentKind::Cubic:
      return NearPoint(lhs.p2, rhs.p2, tolerance) && NearPoint(lhs.p3, rhs.p3, tolerance);
  }
  return false;
}

bool SegmentsNearEqualOppositeDirection(const Segment& lhs, const Segment& rhs, double tolerance) {
  if (lhs.kind != rhs.kind) {
    return false;
  }

  switch (lhs.kind) {
    case SegmentKind::Line:
      return NearPoint(lhs.p0, rhs.p1, tolerance) && NearPoint(lhs.p1, rhs.p0, tolerance);
    case SegmentKind::Quad:
      return NearPoint(lhs.p0, rhs.p2, tolerance) && NearPoint(lhs.p1, rhs.p1, tolerance) &&
             NearPoint(lhs.p2, rhs.p0, tolerance);
    case SegmentKind::Cubic:
      return NearPoint(lhs.p0, rhs.p3, tolerance) && NearPoint(lhs.p1, rhs.p2, tolerance) &&
             NearPoint(lhs.p2, rhs.p1, tolerance) && NearPoint(lhs.p3, rhs.p0, tolerance);
  }
  return false;
}

std::optional<std::vector<Intersection>> IntersectCoincidentFullSegments(const Segment& lhs,
                                                                         const Segment& rhs,
                                                                         double tolerance) {
  std::vector<Intersection> intersections;
  if (SegmentsNearEqualSameDirection(lhs, rhs, tolerance)) {
    AddIntersection(&intersections, 0.0, 0.0, tolerance);
    AddIntersection(&intersections, 1.0, 1.0, tolerance);
    return intersections;
  }
  if (SegmentsNearEqualOppositeDirection(lhs, rhs, tolerance)) {
    AddIntersection(&intersections, 0.0, 1.0, tolerance);
    AddIntersection(&intersections, 1.0, 0.0, tolerance);
    return intersections;
  }
  return std::nullopt;
}

std::optional<Segment> SegmentSpanForComparison(const Segment& segment, double t0, double t1,
                                                double tolerance) {
  if (t1 - t0 <= tolerance) {
    return std::nullopt;
  }

  switch (segment.kind) {
    case SegmentKind::Line:
      return Segment{
          .kind = SegmentKind::Line,
          .p0 = SegmentPointAt(segment, t0),
          .p1 = SegmentPointAt(segment, t1),
      };
    case SegmentKind::Quad: {
      const std::array<Vector2d, 3> quad = SubQuadratic(segment, t0, t1);
      return Segment{.kind = SegmentKind::Quad, .p0 = quad[0], .p1 = quad[1], .p2 = quad[2]};
    }
    case SegmentKind::Cubic: {
      const std::array<Vector2d, 4> cubic = SubCubic(segment, t0, t1);
      return Segment{
          .kind = SegmentKind::Cubic,
          .p0 = cubic[0],
          .p1 = cubic[1],
          .p2 = cubic[2],
          .p3 = cubic[3],
      };
    }
  }
  return std::nullopt;
}

void AddRoot(std::vector<double>* roots, double t, double tolerance) {
  if (t < -tolerance || t > 1.0 + tolerance) {
    return;
  }
  const double clamped = ClampUnit(t);
  for (double existing : *roots) {
    if (std::abs(existing - clamped) <= tolerance) {
      return;
    }
  }
  roots->push_back(clamped);
}

double PolynomialValue(double a, double b, double c, double d, double t) {
  return ((a * t + b) * t + c) * t + d;
}

std::vector<double> QuadraticRoots(double a, double b, double c, double tolerance) {
  std::vector<double> roots;
  const double scale = std::max({1.0, std::abs(a), std::abs(b), std::abs(c)});
  const double coefficientTolerance = tolerance * scale;

  if (std::abs(a) <= coefficientTolerance) {
    if (std::abs(b) > coefficientTolerance) {
      AddRoot(&roots, -c / b, tolerance);
    }
    return roots;
  }

  const double discriminant = b * b - 4.0 * a * c;
  if (discriminant < -coefficientTolerance) {
    return roots;
  }

  if (std::abs(discriminant) <= coefficientTolerance) {
    AddRoot(&roots, -b / (2.0 * a), tolerance);
    return roots;
  }

  const double sqrtDiscriminant = std::sqrt(discriminant);
  AddRoot(&roots, (-b + sqrtDiscriminant) / (2.0 * a), tolerance);
  AddRoot(&roots, (-b - sqrtDiscriminant) / (2.0 * a), tolerance);
  return roots;
}

std::vector<double> CubicRootsInUnitInterval(double a, double b, double c, double d,
                                             double valueTolerance, double rootTolerance) {
  const double scale = std::max({1.0, std::abs(a), std::abs(b), std::abs(c), std::abs(d)});
  const double coefficientTolerance = valueTolerance * scale;
  if (std::abs(a) <= coefficientTolerance) {
    return QuadraticRoots(b, c, d, rootTolerance);
  }

  std::vector<double> roots;
  std::vector<double> bounds = {0.0, 1.0};
  for (double t : QuadraticRoots(3.0 * a, 2.0 * b, c, rootTolerance)) {
    if (t > rootTolerance && t < 1.0 - rootTolerance) {
      bounds.push_back(t);
    }
  }
  std::sort(bounds.begin(), bounds.end());
  bounds.erase(
      std::unique(bounds.begin(), bounds.end(),
                  [&](double lhs, double rhs) { return std::abs(lhs - rhs) <= rootTolerance; }),
      bounds.end());

  for (double t : bounds) {
    if (std::abs(PolynomialValue(a, b, c, d, t)) <= valueTolerance) {
      AddRoot(&roots, t, rootTolerance);
    }
  }

  for (std::size_t i = 1; i < bounds.size(); ++i) {
    double left = bounds[i - 1];
    double right = bounds[i];
    double leftValue = PolynomialValue(a, b, c, d, left);
    double rightValue = PolynomialValue(a, b, c, d, right);
    if (leftValue * rightValue > 0.0) {
      continue;
    }

    for (int iteration = 0; iteration < 64; ++iteration) {
      const double mid = (left + right) * 0.5;
      const double midValue = PolynomialValue(a, b, c, d, mid);
      if (std::abs(midValue) <= valueTolerance || right - left <= rootTolerance) {
        AddRoot(&roots, mid, rootTolerance);
        break;
      }
      if (leftValue * midValue <= 0.0) {
        right = mid;
        rightValue = midValue;
      } else {
        left = mid;
        leftValue = midValue;
      }
    }
  }

  std::sort(roots.begin(), roots.end());
  return roots;
}

std::optional<double> SegmentParameterForPoint(const Segment& segment, const Vector2d& point,
                                               double tolerance) {
  std::vector<double> roots;
  AddRoot(&roots, 0.0, tolerance);
  AddRoot(&roots, 1.0, tolerance);

  if (segment.kind == SegmentKind::Line) {
    const Vector2d direction = segment.p1 - segment.p0;
    const double lengthSquared = direction.lengthSquared();
    if (lengthSquared <= tolerance * tolerance) {
      return std::nullopt;
    }
    AddRoot(&roots, (point - segment.p0).dot(direction) / lengthSquared, tolerance);
  } else {
    const Box2d bounds = SegmentBounds(segment);
    const bool useX = std::abs(bounds.width()) >= std::abs(bounds.height());
    const auto coord = [useX](const Vector2d& vector) { return useX ? vector.x : vector.y; };
    const double rootTolerance = tolerance * 0.01;

    switch (segment.kind) {
      case SegmentKind::Line: break;
      case SegmentKind::Quad: {
        const double a = coord(segment.p0) - 2.0 * coord(segment.p1) + coord(segment.p2);
        const double b = 2.0 * (coord(segment.p1) - coord(segment.p0));
        const double c = coord(segment.p0) - coord(point);
        for (double root : QuadraticRoots(a, b, c, rootTolerance)) {
          AddRoot(&roots, root, rootTolerance);
        }
        break;
      }
      case SegmentKind::Cubic: {
        const double a = coord(segment.p3) - 3.0 * coord(segment.p2) + 3.0 * coord(segment.p1) -
                         coord(segment.p0);
        const double b = 3.0 * (coord(segment.p2) - 2.0 * coord(segment.p1) + coord(segment.p0));
        const double c = 3.0 * (coord(segment.p1) - coord(segment.p0));
        const double d = coord(segment.p0) - coord(point);
        for (double root : CubicRootsInUnitInterval(a, b, c, d, tolerance * 16.0, rootTolerance)) {
          AddRoot(&roots, root, rootTolerance);
        }
        break;
      }
    }
  }

  std::optional<double> bestT;
  double bestDistanceSquared = std::numeric_limits<double>::infinity();
  const double pointTolerance = std::max(tolerance * 64.0, 1e-9);
  for (double root : roots) {
    const Vector2d candidate = SegmentPointAt(segment, root);
    const double distanceSquared = candidate.distanceSquared(point);
    if (distanceSquared <= pointTolerance * pointTolerance &&
        distanceSquared < bestDistanceSquared) {
      bestDistanceSquared = distanceSquared;
      bestT = root;
    }
  }
  return bestT;
}

std::optional<std::vector<Intersection>> IntersectContainedCoincidentSegment(
    const Segment& container, const Segment& contained, bool containerIsFirst, double tolerance) {
  const std::optional<double> containedStartInContainer =
      SegmentParameterForPoint(container, contained.p0, tolerance);
  const std::optional<double> containedEndInContainer =
      SegmentParameterForPoint(container, SegmentPointAt(contained, 1.0), tolerance);
  if (!containedStartInContainer.has_value() || !containedEndInContainer.has_value()) {
    return std::nullopt;
  }

  const double compareTolerance = std::max(tolerance * 64.0, 1e-9);
  std::vector<Intersection> intersections;

  if (*containedStartInContainer + tolerance < *containedEndInContainer) {
    std::optional<Segment> containerSpan = SegmentSpanForComparison(
        container, *containedStartInContainer, *containedEndInContainer, tolerance);
    if (containerSpan.has_value() &&
        SegmentsNearEqualSameDirection(*containerSpan, contained, compareTolerance)) {
      if (containerIsFirst) {
        AddIntersection(&intersections, *containedStartInContainer, 0.0, tolerance);
        AddIntersection(&intersections, *containedEndInContainer, 1.0, tolerance);
      } else {
        AddIntersection(&intersections, 0.0, *containedStartInContainer, tolerance);
        AddIntersection(&intersections, 1.0, *containedEndInContainer, tolerance);
      }
      return intersections;
    }
  }

  if (*containedEndInContainer + tolerance < *containedStartInContainer) {
    std::optional<Segment> containerSpan = SegmentSpanForComparison(
        container, *containedEndInContainer, *containedStartInContainer, tolerance);
    if (containerSpan.has_value() &&
        SegmentsNearEqualOppositeDirection(*containerSpan, contained, compareTolerance)) {
      if (containerIsFirst) {
        AddIntersection(&intersections, *containedEndInContainer, 1.0, tolerance);
        AddIntersection(&intersections, *containedStartInContainer, 0.0, tolerance);
      } else {
        AddIntersection(&intersections, 1.0, *containedEndInContainer, tolerance);
        AddIntersection(&intersections, 0.0, *containedStartInContainer, tolerance);
      }
      return intersections;
    }
  }

  return std::nullopt;
}

std::optional<std::vector<Intersection>> IntersectSameDirectionCoincidentOverlap(const Segment& lhs,
                                                                                 const Segment& rhs,
                                                                                 double tolerance) {
  const std::optional<double> lhsStartInRhs = SegmentParameterForPoint(rhs, lhs.p0, tolerance);
  const std::optional<double> lhsEndInRhs =
      SegmentParameterForPoint(rhs, SegmentPointAt(lhs, 1.0), tolerance);
  const std::optional<double> rhsStartInLhs = SegmentParameterForPoint(lhs, rhs.p0, tolerance);
  const std::optional<double> rhsEndInLhs =
      SegmentParameterForPoint(lhs, SegmentPointAt(rhs, 1.0), tolerance);

  const double compareTolerance = std::max(tolerance * 64.0, 1e-9);
  std::vector<Intersection> intersections;

  if (rhsStartInLhs.has_value() && lhsEndInRhs.has_value() && *rhsStartInLhs + tolerance < 1.0 &&
      *lhsEndInRhs > tolerance) {
    std::optional<Segment> lhsSpan = SegmentSpanForComparison(lhs, *rhsStartInLhs, 1.0, tolerance);
    std::optional<Segment> rhsSpan = SegmentSpanForComparison(rhs, 0.0, *lhsEndInRhs, tolerance);
    if (lhsSpan.has_value() && rhsSpan.has_value() &&
        SegmentsNearEqualSameDirection(*lhsSpan, *rhsSpan, compareTolerance)) {
      AddIntersection(&intersections, *rhsStartInLhs, 0.0, tolerance);
      AddIntersection(&intersections, 1.0, *lhsEndInRhs, tolerance);
      return intersections;
    }
  }

  if (lhsStartInRhs.has_value() && rhsEndInLhs.has_value() && *lhsStartInRhs + tolerance < 1.0 &&
      *rhsEndInLhs > tolerance) {
    std::optional<Segment> lhsSpan = SegmentSpanForComparison(lhs, 0.0, *rhsEndInLhs, tolerance);
    std::optional<Segment> rhsSpan = SegmentSpanForComparison(rhs, *lhsStartInRhs, 1.0, tolerance);
    if (lhsSpan.has_value() && rhsSpan.has_value() &&
        SegmentsNearEqualSameDirection(*lhsSpan, *rhsSpan, compareTolerance)) {
      AddIntersection(&intersections, 0.0, *lhsStartInRhs, tolerance);
      AddIntersection(&intersections, *rhsEndInLhs, 1.0, tolerance);
      return intersections;
    }
  }

  return std::nullopt;
}

std::optional<std::vector<Intersection>> IntersectOppositeDirectionCoincidentOverlap(
    const Segment& lhs, const Segment& rhs, double tolerance) {
  const std::optional<double> lhsStartInRhs = SegmentParameterForPoint(rhs, lhs.p0, tolerance);
  const std::optional<double> lhsEndInRhs =
      SegmentParameterForPoint(rhs, SegmentPointAt(lhs, 1.0), tolerance);
  const std::optional<double> rhsStartInLhs = SegmentParameterForPoint(lhs, rhs.p0, tolerance);
  const std::optional<double> rhsEndInLhs =
      SegmentParameterForPoint(lhs, SegmentPointAt(rhs, 1.0), tolerance);

  const double compareTolerance = std::max(tolerance * 64.0, 1e-9);
  std::vector<Intersection> intersections;

  if (rhsEndInLhs.has_value() && lhsEndInRhs.has_value() && *rhsEndInLhs + tolerance < 1.0 &&
      *lhsEndInRhs + tolerance < 1.0) {
    std::optional<Segment> lhsSpan = SegmentSpanForComparison(lhs, *rhsEndInLhs, 1.0, tolerance);
    std::optional<Segment> rhsSpan = SegmentSpanForComparison(rhs, *lhsEndInRhs, 1.0, tolerance);
    if (lhsSpan.has_value() && rhsSpan.has_value() &&
        SegmentsNearEqualOppositeDirection(*lhsSpan, *rhsSpan, compareTolerance)) {
      AddIntersection(&intersections, *rhsEndInLhs, 1.0, tolerance);
      AddIntersection(&intersections, 1.0, *lhsEndInRhs, tolerance);
      return intersections;
    }
  }

  if (rhsStartInLhs.has_value() && lhsStartInRhs.has_value() && *rhsStartInLhs > tolerance &&
      *lhsStartInRhs > tolerance) {
    std::optional<Segment> lhsSpan = SegmentSpanForComparison(lhs, 0.0, *rhsStartInLhs, tolerance);
    std::optional<Segment> rhsSpan = SegmentSpanForComparison(rhs, 0.0, *lhsStartInRhs, tolerance);
    if (lhsSpan.has_value() && rhsSpan.has_value() &&
        SegmentsNearEqualOppositeDirection(*lhsSpan, *rhsSpan, compareTolerance)) {
      AddIntersection(&intersections, 0.0, *lhsStartInRhs, tolerance);
      AddIntersection(&intersections, *rhsStartInLhs, 0.0, tolerance);
      return intersections;
    }
  }

  return std::nullopt;
}

std::optional<std::vector<Intersection>> IntersectCoincidentPartialSegments(const Segment& lhs,
                                                                            const Segment& rhs,
                                                                            double tolerance) {
  if (lhs.kind != rhs.kind) {
    return std::nullopt;
  }

  if (std::optional<std::vector<Intersection>> intersections =
          IntersectContainedCoincidentSegment(lhs, rhs, true, tolerance)) {
    return intersections;
  }
  if (std::optional<std::vector<Intersection>> intersections =
          IntersectContainedCoincidentSegment(rhs, lhs, false, tolerance)) {
    return intersections;
  }
  if (std::optional<std::vector<Intersection>> intersections =
          IntersectSameDirectionCoincidentOverlap(lhs, rhs, tolerance)) {
    return intersections;
  }
  if (std::optional<std::vector<Intersection>> intersections =
          IntersectOppositeDirectionCoincidentOverlap(lhs, rhs, tolerance)) {
    return intersections;
  }
  return std::nullopt;
}

std::vector<Intersection> IntersectLineLine(const Segment& lhs, const Segment& rhs,
                                            double tolerance) {
  std::vector<Intersection> intersections;
  const Vector2d p = lhs.p0;
  const Vector2d r = lhs.p1 - lhs.p0;
  const Vector2d q = rhs.p0;
  const Vector2d s = rhs.p1 - rhs.p0;
  const double denominator = r.cross(s);
  const Vector2d qMinusP = q - p;

  if (std::abs(denominator) <= tolerance) {
    if (std::abs(qMinusP.cross(r)) > tolerance) {
      return intersections;
    }

    const double rLengthSquared = r.lengthSquared();
    const double sLengthSquared = s.lengthSquared();
    if (rLengthSquared <= tolerance * tolerance || sLengthSquared <= tolerance * tolerance) {
      return intersections;
    }

    double lhsT0 = qMinusP.dot(r) / rLengthSquared;
    double lhsT1 = (q + s - p).dot(r) / rLengthSquared;
    if (lhsT0 > lhsT1) {
      std::swap(lhsT0, lhsT1);
    }

    const double overlapStart = std::max(0.0, lhsT0);
    const double overlapEnd = std::min(1.0, lhsT1);
    if (overlapEnd + tolerance < overlapStart) {
      return intersections;
    }

    const auto rhsParameterForPoint = [&](const Vector2d& point) {
      return (point - q).dot(s) / sLengthSquared;
    };

    const Vector2d startPoint = SegmentPointAt(lhs, overlapStart);
    const Vector2d endPoint = SegmentPointAt(lhs, overlapEnd);
    AddIntersection(&intersections, overlapStart, rhsParameterForPoint(startPoint), tolerance);
    AddIntersection(&intersections, overlapEnd, rhsParameterForPoint(endPoint), tolerance);
    return intersections;
  }

  const double t = qMinusP.cross(s) / denominator;
  const double u = qMinusP.cross(r) / denominator;
  if (t >= -tolerance && t <= 1.0 + tolerance && u >= -tolerance && u <= 1.0 + tolerance) {
    AddIntersection(&intersections, t, u, tolerance);
  }
  return intersections;
}

std::vector<Intersection> IntersectLineCurve(const Segment& lineSegment,
                                             const Segment& curveSegment, bool lineIsFirst,
                                             double tolerance) {
  std::vector<Intersection> intersections;
  const Vector2d lineDirection = lineSegment.p1 - lineSegment.p0;
  const double lineLengthSquared = lineDirection.lengthSquared();
  if (lineLengthSquared <= tolerance * tolerance) {
    return intersections;
  }

  std::vector<double> roots;
  const double valueTolerance = tolerance * std::sqrt(lineLengthSquared);
  const double rootTolerance = tolerance * 0.01;
  switch (curveSegment.kind) {
    case SegmentKind::Line: return IntersectLineLine(lineSegment, curveSegment, tolerance);
    case SegmentKind::Quad: {
      const Vector2d a = curveSegment.p0 - 2.0 * curveSegment.p1 + curveSegment.p2;
      const Vector2d b = 2.0 * (curveSegment.p1 - curveSegment.p0);
      const Vector2d c = curveSegment.p0 - lineSegment.p0;
      roots = QuadraticRoots(a.cross(lineDirection), b.cross(lineDirection), c.cross(lineDirection),
                             rootTolerance);
      break;
    }
    case SegmentKind::Cubic: {
      const Vector2d a =
          curveSegment.p3 - 3.0 * curveSegment.p2 + 3.0 * curveSegment.p1 - curveSegment.p0;
      const Vector2d b = 3.0 * (curveSegment.p2 - 2.0 * curveSegment.p1 + curveSegment.p0);
      const Vector2d c = 3.0 * (curveSegment.p1 - curveSegment.p0);
      const Vector2d d = curveSegment.p0 - lineSegment.p0;
      roots = CubicRootsInUnitInterval(a.cross(lineDirection), b.cross(lineDirection),
                                       c.cross(lineDirection), d.cross(lineDirection),
                                       valueTolerance, rootTolerance);
      break;
    }
  }

  for (double curveT : roots) {
    const Vector2d point = SegmentPointAt(curveSegment, curveT);
    const double lineT = (point - lineSegment.p0).dot(lineDirection) / lineLengthSquared;
    const double distanceToLine =
        std::abs((point - lineSegment.p0).cross(lineDirection)) / std::sqrt(lineLengthSquared);
    if (lineT >= -tolerance && lineT <= 1.0 + tolerance && distanceToLine <= tolerance * 8.0) {
      if (lineIsFirst) {
        AddIntersection(&intersections, lineT, curveT, tolerance);
      } else {
        AddIntersection(&intersections, curveT, lineT, tolerance);
      }
    }
  }
  return intersections;
}

CubicSlice LeftSlice(const CubicSlice& slice, double t) {
  const auto split =
      SplitCubic(slice.points[0], slice.points[1], slice.points[2], slice.points[3], t);
  const double midT = slice.t0 + (slice.t1 - slice.t0) * t;
  return {
      .points = split.first,
      .t0 = slice.t0,
      .t1 = midT,
  };
}

CubicSlice RightSlice(const CubicSlice& slice, double t) {
  const auto split =
      SplitCubic(slice.points[0], slice.points[1], slice.points[2], slice.points[3], t);
  const double midT = slice.t0 + (slice.t1 - slice.t0) * t;
  return {
      .points = split.second,
      .t0 = midT,
      .t1 = slice.t1,
  };
}

bool RefineCubicIntersection(const CubicSlice& lhs, const CubicSlice& rhs, double tolerance,
                             double* lhsT, double* rhsT) {
  double localLhsT = 0.5;
  double localRhsT = 0.5;

  for (int i = 0; i < 12; ++i) {
    const Vector2d lhsPoint =
        EvalCubic(lhs.points[0], lhs.points[1], lhs.points[2], lhs.points[3], localLhsT);
    const Vector2d rhsPoint =
        EvalCubic(rhs.points[0], rhs.points[1], rhs.points[2], rhs.points[3], localRhsT);
    const Vector2d delta = lhsPoint - rhsPoint;
    if (delta.lengthSquared() <= tolerance * tolerance) {
      break;
    }

    const Vector2d lhsDerivative = EvalCubicDerivative(lhs.points, localLhsT);
    const Vector2d rhsDerivative = EvalCubicDerivative(rhs.points, localRhsT);
    const Vector2d negativeDelta = -delta;
    const Vector2d negativeRhsDerivative = -rhsDerivative;
    const double determinant = lhsDerivative.cross(negativeRhsDerivative);
    const double determinantTolerance =
        std::numeric_limits<double>::epsilon() *
        std::max(1.0, lhsDerivative.length() * rhsDerivative.length()) * 64.0;
    if (std::abs(determinant) <= determinantTolerance) {
      break;
    }

    const double lhsStep = negativeDelta.cross(negativeRhsDerivative) / determinant;
    const double rhsStep = lhsDerivative.cross(negativeDelta) / determinant;
    localLhsT = ClampUnit(localLhsT + lhsStep);
    localRhsT = ClampUnit(localRhsT + rhsStep);
  }

  const Vector2d lhsPoint =
      EvalCubic(lhs.points[0], lhs.points[1], lhs.points[2], lhs.points[3], localLhsT);
  const Vector2d rhsPoint =
      EvalCubic(rhs.points[0], rhs.points[1], rhs.points[2], rhs.points[3], localRhsT);
  if (!NearPoint(lhsPoint, rhsPoint, tolerance * 8.0)) {
    return false;
  }

  *lhsT = lhs.t0 + (lhs.t1 - lhs.t0) * localLhsT;
  *rhsT = rhs.t0 + (rhs.t1 - rhs.t0) * localRhsT;
  return true;
}

bool IntersectCubicRecursive(const CubicSlice& lhs, const CubicSlice& rhs, double tolerance,
                             int depth, CubicIntersectionSearch* search) {
  constexpr int kMaxDepth = 26;
  if (search->budget->steps >= search->budget->maxSteps) {
    return false;
  }

  ++search->budget->steps;
  const Box2d lhsBounds = CubicBoundsOf(lhs.points);
  const Box2d rhsBounds = CubicBoundsOf(rhs.points);
  if (!BoxesOverlap(lhsBounds, rhsBounds, tolerance)) {
    return true;
  }

  if (depth >= kMaxDepth ||
      (BoxMaxExtent(lhsBounds) <= tolerance && BoxMaxExtent(rhsBounds) <= tolerance)) {
    double lhsT = (lhs.t0 + lhs.t1) * 0.5;
    double rhsT = (rhs.t0 + rhs.t1) * 0.5;
    if (RefineCubicIntersection(lhs, rhs, tolerance, &lhsT, &rhsT)) {
      return AddBoundedIntersection(search, lhsT, rhsT, tolerance * 16.0);
    }
    return true;
  }

  if (BoxMaxExtent(lhsBounds) >= BoxMaxExtent(rhsBounds)) {
    return IntersectCubicRecursive(LeftSlice(lhs, 0.5), rhs, tolerance, depth + 1, search) &&
           IntersectCubicRecursive(RightSlice(lhs, 0.5), rhs, tolerance, depth + 1, search);
  } else {
    return IntersectCubicRecursive(lhs, LeftSlice(rhs, 0.5), tolerance, depth + 1, search) &&
           IntersectCubicRecursive(lhs, RightSlice(rhs, 0.5), tolerance, depth + 1, search);
  }
}

SegmentIntersectionResult IntersectSegments(const Segment& lhs, const Segment& rhs,
                                            double tolerance, std::size_t maxIntersections,
                                            IntersectionSearchBudget* searchBudget) {
  if (std::optional<std::vector<Intersection>> coincident =
          IntersectCoincidentFullSegments(lhs, rhs, tolerance)) {
    return {.intersections = *coincident};
  }
  if (std::optional<std::vector<Intersection>> coincident =
          IntersectCoincidentPartialSegments(lhs, rhs, tolerance)) {
    return {.intersections = *coincident};
  }
  if (lhs.kind == SegmentKind::Line && rhs.kind == SegmentKind::Line) {
    return {.intersections = IntersectLineLine(lhs, rhs, tolerance)};
  }
  if (lhs.kind == SegmentKind::Line) {
    return {.intersections = IntersectLineCurve(lhs, rhs, true, tolerance)};
  }
  if (rhs.kind == SegmentKind::Line) {
    return {.intersections = IntersectLineCurve(rhs, lhs, false, tolerance)};
  }

  CubicIntersectionSearch search{
      .budget = searchBudget,
      .maxIntersections = maxIntersections,
  };
  const CubicSlice lhsSlice{.points = SegmentAsCubic(lhs), .t0 = 0.0, .t1 = 1.0};
  const CubicSlice rhsSlice{.points = SegmentAsCubic(rhs), .t0 = 0.0, .t1 = 1.0};
  if (!IntersectCubicRecursive(lhsSlice, rhsSlice, tolerance, 0, &search)) {
    return {.tooComplex = true};
  }

  return {.intersections = std::move(search.intersections)};
}

std::array<Vector2d, 3> SubQuadratic(const Segment& segment, double t0, double t1) {
  if (t0 <= 0.0 && t1 >= 1.0) {
    return {segment.p0, segment.p1, segment.p2};
  }

  const auto firstSplit = SplitQuadratic(segment.p0, segment.p1, segment.p2, t1);
  if (t0 <= 0.0) {
    return firstSplit.first;
  }

  const double relativeT = t0 / t1;
  const auto secondSplit =
      SplitQuadratic(firstSplit.first[0], firstSplit.first[1], firstSplit.first[2], relativeT);
  return secondSplit.second;
}

std::array<Vector2d, 4> SubCubic(const Segment& segment, double t0, double t1) {
  if (t0 <= 0.0 && t1 >= 1.0) {
    return {segment.p0, segment.p1, segment.p2, segment.p3};
  }

  const auto firstSplit = SplitCubic(segment.p0, segment.p1, segment.p2, segment.p3, t1);
  if (t0 <= 0.0) {
    return firstSplit.first;
  }

  const double relativeT = t0 / t1;
  const auto secondSplit = SplitCubic(firstSplit.first[0], firstSplit.first[1], firstSplit.first[2],
                                      firstSplit.first[3], relativeT);
  return secondSplit.second;
}

void SetEdgeEndPoint(Edge* edge, const Vector2d& point) {
  switch (edge->kind) {
    case SegmentKind::Line: edge->p1 = point; break;
    case SegmentKind::Quad: edge->p2 = point; break;
    case SegmentKind::Cubic: edge->p3 = point; break;
  }
}

std::optional<Edge> SplitSegmentSpan(const Segment& segment, const SegmentSplit& split0,
                                     const SegmentSplit& split1, double tolerance) {
  const double t0 = split0.t;
  const double t1 = split1.t;
  if (t1 - t0 <= tolerance) {
    return std::nullopt;
  }

  Edge edge;
  switch (segment.kind) {
    case SegmentKind::Line:
      edge = Edge{
          .kind = SegmentKind::Line,
          .p0 = Lerp(segment.p0, segment.p1, t0),
          .p1 = Lerp(segment.p0, segment.p1, t1),
      };
      break;
    case SegmentKind::Quad: {
      const std::array<Vector2d, 3> quad = SubQuadratic(segment, t0, t1);
      edge = Edge{.kind = SegmentKind::Quad, .p0 = quad[0], .p1 = quad[1], .p2 = quad[2]};
      break;
    }
    case SegmentKind::Cubic: {
      const std::array<Vector2d, 4> cubic = SubCubic(segment, t0, t1);
      edge = Edge{
          .kind = SegmentKind::Cubic,
          .p0 = cubic[0],
          .p1 = cubic[1],
          .p2 = cubic[2],
          .p3 = cubic[3],
      };
      break;
    }
  }

  if (split0.point.has_value()) {
    edge.p0 = *split0.point;
  }
  if (split1.point.has_value()) {
    SetEdgeEndPoint(&edge, *split1.point);
  }
  return edge;
}

Edge ReverseEdge(const Edge& edge) {
  switch (edge.kind) {
    case SegmentKind::Line: return Edge{.kind = SegmentKind::Line, .p0 = edge.p1, .p1 = edge.p0};
    case SegmentKind::Quad:
      return Edge{.kind = SegmentKind::Quad, .p0 = edge.p2, .p1 = edge.p1, .p2 = edge.p0};
    case SegmentKind::Cubic:
      return Edge{
          .kind = SegmentKind::Cubic, .p0 = edge.p3, .p1 = edge.p2, .p2 = edge.p1, .p3 = edge.p0};
  }
  return edge;
}

Path TransformPath(const Path& path, const Transform2d& outputFromPath) {
  PathBuilder builder;
  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo: builder.moveTo(outputFromPath.transformPosition(points[0])); break;
      case Path::Verb::LineTo: builder.lineTo(outputFromPath.transformPosition(points[0])); break;
      case Path::Verb::QuadTo:
        builder.quadTo(outputFromPath.transformPosition(points[0]),
                       outputFromPath.transformPosition(points[1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(outputFromPath.transformPosition(points[0]),
                        outputFromPath.transformPosition(points[1]),
                        outputFromPath.transformPosition(points[2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  });
  return builder.build();
}

void CloseContourIfNeeded(std::vector<Segment>* segments, const Vector2d& currentPoint,
                          const Vector2d& contourStart, std::size_t inputIndex,
                          std::size_t contourIndex, double tolerance) {
  if (!NearPoint(currentPoint, contourStart, tolerance)) {
    segments->push_back(Segment{
        .kind = SegmentKind::Line,
        .p0 = currentPoint,
        .p1 = contourStart,
        .inputIndex = inputIndex,
        .contourIndex = contourIndex,
    });
  }
}

bool AppendSegmentsForPath(const Path& path, std::size_t inputIndex, double tolerance,
                           std::vector<Segment>* segments) {
  Vector2d currentPoint;
  Vector2d contourStart;
  std::size_t contourIndex = 0;
  bool hasOpenContour = false;
  bool contourHasSegments = false;

  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo:
        if (hasOpenContour && contourHasSegments) {
          CloseContourIfNeeded(segments, currentPoint, contourStart, inputIndex, contourIndex,
                               tolerance);
          ++contourIndex;
        }
        contourStart = points[0];
        currentPoint = points[0];
        hasOpenContour = true;
        contourHasSegments = false;
        break;
      case Path::Verb::LineTo:
        segments->push_back(Segment{
            .kind = SegmentKind::Line,
            .p0 = currentPoint,
            .p1 = points[0],
            .inputIndex = inputIndex,
            .contourIndex = contourIndex,
        });
        currentPoint = points[0];
        contourHasSegments = true;
        break;
      case Path::Verb::QuadTo:
        segments->push_back(Segment{
            .kind = SegmentKind::Quad,
            .p0 = currentPoint,
            .p1 = points[0],
            .p2 = points[1],
            .inputIndex = inputIndex,
            .contourIndex = contourIndex,
        });
        currentPoint = points[1];
        contourHasSegments = true;
        break;
      case Path::Verb::CurveTo:
        segments->push_back(Segment{
            .kind = SegmentKind::Cubic,
            .p0 = currentPoint,
            .p1 = points[0],
            .p2 = points[1],
            .p3 = points[2],
            .inputIndex = inputIndex,
            .contourIndex = contourIndex,
        });
        currentPoint = points[2];
        contourHasSegments = true;
        break;
      case Path::Verb::ClosePath:
        if (hasOpenContour && contourHasSegments) {
          CloseContourIfNeeded(segments, currentPoint, contourStart, inputIndex, contourIndex,
                               tolerance);
          ++contourIndex;
        }
        currentPoint = contourStart;
        hasOpenContour = false;
        contourHasSegments = false;
        break;
    }
  });

  if (hasOpenContour && contourHasSegments) {
    CloseContourIfNeeded(segments, currentPoint, contourStart, inputIndex, contourIndex, tolerance);
  }

  for (const Segment& segment : *segments) {
    if (!IsFinite(segment.p0) || !IsFinite(segment.p1) ||
        (segment.kind != SegmentKind::Line && !IsFinite(segment.p2)) ||
        (segment.kind == SegmentKind::Cubic && !IsFinite(segment.p3))) {
      return false;
    }
  }
  return true;
}

bool BooleanValue(PathBooleanOp op, std::span<const InputPath> inputs, const Vector2d& point,
                  double tolerance) {
  std::size_t insideCount = 0;
  bool firstInside = false;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const bool inside = StrictPathContains(inputs[i].path, point, inputs[i].fillRule, tolerance);
    if (i == 0) {
      firstInside = inside;
    }
    if (inside) {
      ++insideCount;
    }
  }

  switch (op) {
    case PathBooleanOp::Union: return insideCount > 0u;
    case PathBooleanOp::Intersect: return insideCount == inputs.size();
    case PathBooleanOp::Difference: return firstInside && insideCount == (firstInside ? 1u : 0u);
    case PathBooleanOp::Xor: return (insideCount % 2u) == 1u;
  }
  return false;
}

std::optional<Edge> ClassifyEdge(const Edge& edge, PathBooleanOp op,
                                 std::span<const InputPath> inputs, double tolerance) {
  const Vector2d tangent = EdgeTangentAt(edge, 0.5).normalize();
  if (NearZero(tangent.lengthSquared())) {
    return std::nullopt;
  }

  const Vector2d normal(-tangent.y, tangent.x);
  const Vector2d midpoint = EdgePointAt(edge, 0.5);
  const std::array<double, 4> sampleDistances = {
      std::max(tolerance * 4.0, 1e-9),
      std::max(0.01, tolerance * 16.0),
      std::max(0.05, tolerance * 64.0),
      std::max(0.25, tolerance * 128.0),
  };
  for (double sampleDistance : sampleDistances) {
    const bool leftInside = BooleanValue(op, inputs, midpoint + normal * sampleDistance, tolerance);
    const bool rightInside =
        BooleanValue(op, inputs, midpoint - normal * sampleDistance, tolerance);

    if (leftInside == rightInside) {
      continue;
    }
    if (leftInside) {
      return edge;
    }
    return ReverseEdge(edge);
  }
  return std::nullopt;
}

PointKey QuantizePoint(const Vector2d& point, double tolerance) {
  const double scale = 1.0 / std::max(tolerance * 256.0, 1e-9);
  return {
      .x = static_cast<std::int64_t>(std::llround(point.x * scale)),
      .y = static_cast<std::int64_t>(std::llround(point.y * scale)),
  };
}

bool PointLess(const Vector2d& lhs, const Vector2d& rhs, double tolerance) {
  if (!NearEquals(lhs.x, rhs.x, tolerance)) {
    return lhs.x < rhs.x;
  }
  return lhs.y < rhs.y - tolerance;
}

double EdgeStartAngle(const Edge& edge) {
  return EdgeTangentAt(edge, 0.0).angle();
}

double PositiveAngleDiff(double fromAngle, double toAngle) {
  double diff = toAngle - fromAngle;
  while (diff < 0.0) {
    diff += MathConstants<double>::kPi * 2.0;
  }
  while (diff >= MathConstants<double>::kPi * 2.0) {
    diff -= MathConstants<double>::kPi * 2.0;
  }
  return diff;
}

std::optional<std::size_t> FindFirstUnusedEdge(const std::vector<TraceEdge>& edges,
                                               double tolerance) {
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (edges[i].used) {
      continue;
    }
    if (!best.has_value() || PointLess(edges[i].edge.p0, edges[*best].edge.p0, tolerance)) {
      best = i;
    }
  }
  return best;
}

bool ContainsIndex(const std::vector<std::size_t>& indices, std::size_t index) {
  return std::find(indices.begin(), indices.end(), index) != indices.end();
}

enum class ContourSearchResult : std::uint8_t {
  Closed,
  Open,
  TooComplex,
};

std::size_t MaxContourSearchSteps(const PathBooleanOptions& options, std::size_t edgeCount) {
  constexpr std::size_t kTraceSearchMultiplier = 16u;
  const std::size_t base = std::max(edgeCount, options.maxOutputCommands);
  if (base > std::numeric_limits<std::size_t>::max() / kTraceSearchMultiplier) {
    return std::numeric_limits<std::size_t>::max();
  }
  return base * kTraceSearchMultiplier;
}

std::vector<std::size_t> CandidateNextEdges(const std::vector<TraceEdge>& edges,
                                            const PointKey& startKey, const Vector2d& startPoint,
                                            double previousAngle, double snapTolerance,
                                            const std::vector<std::size_t>& contourIndices) {
  std::vector<std::size_t> candidates;
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (edges[i].used || ContainsIndex(contourIndices, i)) {
      continue;
    }
    if (!(edges[i].startKey == startKey) &&
        !NearPoint(edges[i].edge.p0, startPoint, snapTolerance)) {
      continue;
    }
    candidates.push_back(i);
  }

  std::sort(candidates.begin(), candidates.end(), [&](std::size_t lhs, std::size_t rhs) {
    const double lhsTurn = PositiveAngleDiff(previousAngle, EdgeStartAngle(edges[lhs].edge));
    const double rhsTurn = PositiveAngleDiff(previousAngle, EdgeStartAngle(edges[rhs].edge));
    if (!NearEquals(lhsTurn, rhsTurn, 1e-12)) {
      return lhsTurn < rhsTurn;
    }
    return PointLess(edges[lhs].edge.p0, edges[rhs].edge.p0, 1e-12);
  });
  return candidates;
}

ContourSearchResult FindClosedContour(const std::vector<TraceEdge>& edges,
                                      const PointKey& contourStartKey,
                                      const Vector2d& contourStartPoint, const PointKey& currentKey,
                                      const Vector2d& currentPoint, double previousAngle,
                                      double snapTolerance,
                                      std::vector<std::size_t>* contourIndices,
                                      std::size_t* searchSteps, std::size_t maxSearchSteps) {
  if (*searchSteps >= maxSearchSteps) {
    return ContourSearchResult::TooComplex;
  }
  ++*searchSteps;

  if (contourIndices->size() > edges.size()) {
    return ContourSearchResult::Open;
  }

  for (std::size_t nextIndex : CandidateNextEdges(edges, currentKey, currentPoint, previousAngle,
                                                  snapTolerance, *contourIndices)) {
    contourIndices->push_back(nextIndex);
    const TraceEdge& nextEdge = edges[nextIndex];
    const Vector2d nextEndPoint = EdgePointAt(nextEdge.edge, 1.0);
    if (nextEdge.endKey == contourStartKey ||
        NearPoint(nextEndPoint, contourStartPoint, snapTolerance)) {
      return ContourSearchResult::Closed;
    }
    const ContourSearchResult result =
        FindClosedContour(edges, contourStartKey, contourStartPoint, nextEdge.endKey, nextEndPoint,
                          EdgeTangentAt(nextEdge.edge, 1.0).angle(), snapTolerance, contourIndices,
                          searchSteps, maxSearchSteps);
    if (result != ContourSearchResult::Open) {
      return result;
    }
    contourIndices->pop_back();
  }

  return ContourSearchResult::Open;
}

void AppendEdge(PathBuilder* builder, const Edge& edge) {
  switch (edge.kind) {
    case SegmentKind::Line: builder->lineTo(edge.p1); break;
    case SegmentKind::Quad: builder->quadTo(edge.p1, edge.p2); break;
    case SegmentKind::Cubic: builder->curveTo(edge.p1, edge.p2, edge.p3); break;
  }
}

PathBooleanResult TraceOutput(std::vector<Edge> classifiedEdges, double tolerance,
                              const PathBooleanOptions& options) {
  std::vector<TraceEdge> edges;
  edges.reserve(classifiedEdges.size());
  for (const Edge& edge : classifiedEdges) {
    if (NearPoint(edge.p0, EdgePointAt(edge, 1.0), tolerance)) {
      continue;
    }
    edges.push_back(TraceEdge{
        .edge = edge,
        .startKey = QuantizePoint(edge.p0, tolerance),
        .endKey = QuantizePoint(EdgePointAt(edge, 1.0), tolerance),
    });
  }

  const std::size_t maxContourSearchSteps = MaxContourSearchSteps(options, edges.size());
  std::size_t contourSearchSteps = 0;

  PathBuilder builder;
  std::size_t outputCommands = 0;
  std::size_t droppedOpenContours = 0;
  while (std::optional<std::size_t> first = FindFirstUnusedEdge(edges, tolerance)) {
    const Vector2d contourStart = edges[*first].edge.p0;
    const PointKey contourStartKey = edges[*first].startKey;
    const Vector2d firstEndPoint = EdgePointAt(edges[*first].edge, 1.0);
    const double snapTolerance = std::max(tolerance * 512.0, tolerance);
    std::vector<std::size_t> contourIndices = {*first};
    bool contourClosed = edges[*first].endKey == contourStartKey ||
                         NearPoint(firstEndPoint, contourStart, snapTolerance);
    if (!contourClosed) {
      const ContourSearchResult searchResult = FindClosedContour(
          edges, contourStartKey, contourStart, edges[*first].endKey, firstEndPoint,
          EdgeTangentAt(edges[*first].edge, 1.0).angle(), snapTolerance, &contourIndices,
          &contourSearchSteps, maxContourSearchSteps);
      if (searchResult == ContourSearchResult::TooComplex) {
        return {
            .status = PathBooleanStatus::TooComplex,
            .diagnostics = {"Path boolean trace exceeded contour search limit"},
        };
      }
      contourClosed = searchResult == ContourSearchResult::Closed;
    }
    if (!contourClosed) {
      edges[*first].used = true;
      ++droppedOpenContours;
      continue;
    }

    std::size_t contourCommandCount = 2u;
    for (std::size_t edgeIndex : contourIndices) {
      const Edge& edge = edges[edgeIndex].edge;
      const bool closesWithLine =
          QuantizePoint(EdgePointAt(edge, 1.0), tolerance) == contourStartKey &&
          edge.kind == SegmentKind::Line && NearPoint(edge.p1, contourStart, tolerance);
      if (!closesWithLine) {
        ++contourCommandCount;
      }
    }
    if (outputCommands + contourCommandCount > options.maxOutputCommands) {
      return {
          .status = PathBooleanStatus::TooComplex,
          .diagnostics = {"Path boolean output exceeded maxOutputCommands"},
      };
    }

    builder.moveTo(contourStart);
    ++outputCommands;
    for (std::size_t edgeIndex : contourIndices) {
      TraceEdge& traceEdge = edges[edgeIndex];
      traceEdge.used = true;
      const Edge& edge = traceEdge.edge;
      const bool closesWithLine =
          QuantizePoint(EdgePointAt(edge, 1.0), tolerance) == contourStartKey &&
          edge.kind == SegmentKind::Line && NearPoint(edge.p1, contourStart, tolerance);
      if (!closesWithLine) {
        AppendEdge(&builder, edge);
        ++outputCommands;
      }
    }
    builder.closePath();
    ++outputCommands;
  }

  Path path = builder.build();
  if (path.empty()) {
    return {
        .status = PathBooleanStatus::EmptyResult,
        .diagnostics = {"Path boolean is empty after tracing " + std::to_string(edges.size()) +
                        " edges and dropping " + std::to_string(droppedOpenContours) +
                        " open contours"},
    };
  }
  return {.status = PathBooleanStatus::Ok, .paths = {std::move(path)}};
}

}  // namespace

PathBooleanResult ApplyPathBoolean(PathBooleanOp op, std::span<const PathBooleanInput> inputs,
                                   const PathBooleanOptions& options) {
  const double tolerance = NormalizedTolerance(options);
  if (inputs.size() < 2u) {
    return {
        .status = PathBooleanStatus::InvalidInput,
        .diagnostics = {"Path boolean requires at least two inputs"},
    };
  }

  std::vector<InputPath> transformedInputs;
  transformedInputs.reserve(inputs.size());
  std::vector<Segment> segments;

  for (std::size_t i = 0; i < inputs.size(); ++i) {
    Path transformedPath = TransformPath(inputs[i].path, inputs[i].outputFromPath);
    transformedInputs.push_back(InputPath{
        .path = transformedPath,
        .fillRule = inputs[i].fillRule,
    });
    const std::size_t previousSegmentCount = segments.size();
    if (!AppendSegmentsForPath(transformedPath, i, tolerance, &segments)) {
      return {
          .status = PathBooleanStatus::InvalidInput,
          .diagnostics = {"Path boolean input contains non-finite coordinates"},
      };
    }
    if (segments.size() == previousSegmentCount) {
      return {
          .status = PathBooleanStatus::InvalidInput,
          .diagnostics = {"Path boolean input contains no filled contours"},
      };
    }
    if (segments.size() > options.maxCurveCount) {
      return {
          .status = PathBooleanStatus::TooComplex,
          .diagnostics = {"Path boolean input exceeded maxCurveCount"},
      };
    }
  }

  if (std::optional<PathBooleanResult> duplicateResult =
          TryDuplicateInputShortcut(op, transformedInputs, options, tolerance)) {
    return *duplicateResult;
  }

  std::size_t intersectionCount = 0;
  IntersectionSearchBudget intersectionSearchBudget{
      .maxSteps = MaxIntersectionSearchSteps(options, segments.size()),
  };
  for (std::size_t i = 0; i < segments.size(); ++i) {
    for (std::size_t j = i + 1; j < segments.size(); ++j) {
      if (!BoxesOverlap(SegmentBounds(segments[i]), SegmentBounds(segments[j]), tolerance)) {
        continue;
      }

      const std::size_t remainingIntersections = options.maxIntersections > intersectionCount
                                                     ? options.maxIntersections - intersectionCount
                                                     : 0;
      const SegmentIntersectionResult intersectionResult = IntersectSegments(
          segments[i], segments[j], tolerance, remainingIntersections, &intersectionSearchBudget);
      if (intersectionResult.tooComplex) {
        return {
            .status = PathBooleanStatus::TooComplex,
            .diagnostics = {"Path boolean intersection search exceeded complexity budget"},
        };
      }

      intersectionCount += intersectionResult.intersections.size();
      if (intersectionCount > options.maxIntersections) {
        return {
            .status = PathBooleanStatus::TooComplex,
            .diagnostics = {"Path boolean input exceeded maxIntersections"},
        };
      }
      for (const Intersection& intersection : intersectionResult.intersections) {
        const Vector2d lhsPoint = SegmentPointAt(segments[i], intersection.t0);
        const Vector2d rhsPoint = SegmentPointAt(segments[j], intersection.t1);
        const Vector2d snapPoint = (lhsPoint + rhsPoint) * 0.5;
        AddSplit(&segments[i], intersection.t0, snapPoint, tolerance);
        AddSplit(&segments[j], intersection.t1, snapPoint, tolerance);
      }
    }
  }

  std::vector<Edge> classifiedEdges;
  for (Segment& segment : segments) {
    std::sort(segment.splits.begin(), segment.splits.end(),
              [](const SegmentSplit& lhs, const SegmentSplit& rhs) { return lhs.t < rhs.t; });
    for (std::size_t i = 1; i < segment.splits.size(); ++i) {
      std::optional<Edge> edge =
          SplitSegmentSpan(segment, segment.splits[i - 1], segment.splits[i], tolerance);
      if (!edge.has_value()) {
        continue;
      }
      std::optional<Edge> classified =
          ClassifyEdge(*edge, op, std::span<const InputPath>(transformedInputs), tolerance);
      if (classified.has_value()) {
        classifiedEdges.push_back(*classified);
      }
    }
  }

  if (classifiedEdges.empty()) {
    return {
        .status = PathBooleanStatus::EmptyResult,
        .diagnostics = {"Path boolean is empty after classifying " +
                        std::to_string(intersectionCount) + " intersections"},
    };
  }

  return TraceOutput(std::move(classifiedEdges), tolerance, options);
}

}  // namespace donner
