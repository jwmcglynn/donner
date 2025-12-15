#include "donner/backends/tiny_skia_cpp/PathGeometry.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "donner/base/Vector2.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

constexpr double kCurveTolerance = 0.001;
constexpr double kDistanceEpsilon = 1e-6;
constexpr int kDashMaxRecursionDepth = 10;
constexpr int kRoundJoinSegments = 8;

PathPoint ToPoint(const Vector2d& point) {
  return PathPoint{static_cast<float>(point.x), static_cast<float>(point.y)};
}

Vector2d FromPoint(const PathPoint& point) {
  return Vector2d(point.x, point.y);
}

double EvaluateCubic(double p0, double p1, double p2, double p3, double t) {
  const double oneMinusT = 1.0 - t;
  const double oneMinusTSquared = oneMinusT * oneMinusT;
  const double oneMinusTCubed = oneMinusTSquared * oneMinusT;
  const double tSquared = t * t;
  const double tCubed = tSquared * t;

  return p0 * oneMinusTCubed + 3.0 * p1 * oneMinusTSquared * t + 3.0 * p2 * oneMinusT * tSquared +
         p3 * tCubed;
}

double Cross(const Vector2d& a, const Vector2d& b) {
  return a.x * b.y - a.y * b.x;
}

Vector2d Normalize(const Vector2d& v) {
  const double length = v.length();
  if (length <= kDistanceEpsilon) {
    return Vector2d();
  }
  return v / length;
}

Vector2d LeftNormal(const Vector2d& direction, double halfWidth) {
  const Vector2d normalized = Normalize(direction);
  return Vector2d(-normalized.y, normalized.x) * halfWidth;
}

Vector2d RightNormal(const Vector2d& direction, double halfWidth) {
  const Vector2d normalized = Normalize(direction);
  return Vector2d(normalized.y, -normalized.x) * halfWidth;
}

template <typename UpdateFunc>
void UpdateCubicExtrema(double p0, double p1, double p2, double p3, const UpdateFunc& update) {
  const double a = -p0 + 3.0 * p1 - 3.0 * p2 + p3;
  const double b = 2.0 * (p0 - 2.0 * p1 + p2);
  const double c = p1 - p0;

  if (std::abs(a) <= std::numeric_limits<double>::epsilon()) {
    if (std::abs(b) <= std::numeric_limits<double>::epsilon()) {
      return;
    }
    const double tLinear = -c / b;
    if (tLinear > 0.0 && tLinear < 1.0) {
      update(tLinear);
    }
    return;
  }

  const double discriminant = b * b - 4.0 * a * c;
  if (discriminant < 0.0) {
    return;
  }

  const double sqrtDiscriminant = std::sqrt(discriminant);
  const double denom = 2.0 * a;

  const double t1 = (-b + sqrtDiscriminant) / denom;
  const double t2 = (-b - sqrtDiscriminant) / denom;
  if (t1 > 0.0 && t1 < 1.0) {
    update(t1);
  }
  if (t2 > 0.0 && t2 < 1.0) {
    update(t2);
  }
}

void UpdateBoundsWithPoint(const Vector2d& point, Boxd& bounds) {
  bounds.topLeft.x = std::min(bounds.topLeft.x, point.x);
  bounds.topLeft.y = std::min(bounds.topLeft.y, point.y);
  bounds.bottomRight.x = std::max(bounds.bottomRight.x, point.x);
  bounds.bottomRight.y = std::max(bounds.bottomRight.y, point.y);
}

bool IsCurveFlatEnough(const std::array<Vector2d, 4>& points) {
  const double chordLength = (points[3] - points[0]).length();
  const double netLength = (points[1] - points[0]).length() + (points[2] - points[1]).length() +
                           (points[3] - points[2]).length();

  return (netLength - chordLength) <= kCurveTolerance;
}

void FlattenCubic(const std::array<Vector2d, 4>& points, std::vector<Vector2d>& flattened,
                  int depth = 0) {
  if (depth > kDashMaxRecursionDepth || IsCurveFlatEnough(points)) {
    flattened.push_back(points[3]);
    return;
  }

  const Vector2d p01 = (points[0] + points[1]) * 0.5;
  const Vector2d p12 = (points[1] + points[2]) * 0.5;
  const Vector2d p23 = (points[2] + points[3]) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  const std::array<Vector2d, 4> left = {points[0], p01, p012, p0123};
  const std::array<Vector2d, 4> right = {p0123, p123, p23, points[3]};

  FlattenCubic(left, flattened, depth + 1);
  FlattenCubic(right, flattened, depth + 1);
}

Vector2d InterpolatePoint(const Vector2d& start, const Vector2d& end, double t) {
  return start + (end - start) * t;
}

struct DashState {
  size_t index = 0;
  double remaining = 0.0;
  bool draw = true;
};

DashState InitializeDashState(const StrokeDash& dash) {
  DashState state;
  state.index = dash.firstIndex();
  state.remaining = dash.firstLength();
  state.draw = (state.index % 2) == 0;
  return state;
}

void AdvanceDashState(const StrokeDash& dash, DashState& state) {
  state.index = (state.index + 1) % dash.array().size();
  state.remaining = dash.array()[state.index];
  state.draw = (state.index % 2) == 0;
}

void EnsureRemainingDistance(const StrokeDash& dash, DashState& state) {
  while (state.remaining <= kDistanceEpsilon) {
    AdvanceDashState(dash, state);
  }
}

void EmitDashSegment(const Vector2d& start, const Vector2d& end, svg::PathSpline& dashedPath,
                     Vector2d& currentOutPoint, bool& hasOutPoint) {
  if ((end - start).lengthSquared() <= kDistanceEpsilon) {
    return;
  }

  if (!hasOutPoint || (currentOutPoint - start).lengthSquared() > kDistanceEpsilon) {
    dashedPath.moveTo(start);
  }

  dashedPath.lineTo(end);
  currentOutPoint = end;
  hasOutPoint = true;
}

void DashLinearSegment(const Vector2d& start, const Vector2d& end, const StrokeDash& dash,
                       DashState& state, svg::PathSpline& dashedPath, Vector2d& currentOutPoint,
                       bool& hasOutPoint) {
  const double segmentLength = (end - start).length();
  if (segmentLength <= kDistanceEpsilon) {
    return;
  }

  double consumed = 0.0;
  while (consumed + kDistanceEpsilon < segmentLength) {
    EnsureRemainingDistance(dash, state);

    const double step = std::min(segmentLength - consumed, state.remaining);
    const double startT = consumed / segmentLength;
    const double endT = (consumed + step) / segmentLength;

    if (state.draw && step > kDistanceEpsilon) {
      EmitDashSegment(InterpolatePoint(start, end, startT), InterpolatePoint(start, end, endT),
                      dashedPath, currentOutPoint, hasOutPoint);
    }

    consumed += step;
    state.remaining -= step;
    if (state.remaining <= kDistanceEpsilon) {
      AdvanceDashState(dash, state);
    }
  }
}

void DashPolyline(const std::vector<Vector2d>& points, const StrokeDash& dash,
                  svg::PathSpline& dashedPath) {
  if (points.size() < 2) {
    return;
  }

  DashState state = InitializeDashState(dash);
  Vector2d currentOutPoint;
  bool hasOutPoint = false;

  for (size_t i = 1; i < points.size(); ++i) {
    DashLinearSegment(points[i - 1], points[i], dash, state, dashedPath, currentOutPoint,
                      hasOutPoint);
  }
}

void AppendFlattenedCubic(const Vector2d& start, const Vector2d& control1, const Vector2d& control2,
                          const Vector2d& end, std::vector<Vector2d>& polyline) {
  const std::array<Vector2d, 4> points = {start, control1, control2, end};
  FlattenCubic(points, polyline);
}

std::optional<Vector2d> IntersectLines(const Vector2d& point1, const Vector2d& dir1,
                                       const Vector2d& point2, const Vector2d& dir2) {
  const double denom = Cross(dir1, dir2);
  if (std::abs(denom) <= kDistanceEpsilon) {
    return std::nullopt;
  }

  const double t = Cross(point2 - point1, dir2) / denom;
  return point1 + dir1 * t;
}

void AppendArcPoints(const Vector2d& center, const Vector2d& from, const Vector2d& to,
                     bool clockwise, int segments, std::vector<Vector2d>& out,
                     bool includeStart = false) {
  const Vector2d fromNormalized = Normalize(from);
  const Vector2d toNormalized = Normalize(to);
  double startAngle = std::atan2(fromNormalized.y, fromNormalized.x);
  double endAngle = std::atan2(toNormalized.y, toNormalized.x);

  if (clockwise && endAngle > startAngle) {
    endAngle -= 2.0 * M_PI;
  } else if (!clockwise && endAngle < startAngle) {
    endAngle += 2.0 * M_PI;
  }

  const double step = (endAngle - startAngle) / static_cast<double>(segments);

  for (int i = includeStart ? 0 : 1; i <= segments; ++i) {
    const double angle = startAngle + step * i;
    const double cosAngle = std::cos(angle);
    const double sinAngle = std::sin(angle);
    out.push_back(center + Vector2d(cosAngle * from.length(), sinAngle * from.length()));
  }
}

struct Subpath {
  std::vector<Vector2d> points;
  bool closed = false;
};

std::vector<Subpath> FlattenSpline(const svg::PathSpline& spline) {
  std::vector<Subpath> subpaths;
  Subpath current;

  const auto& commands = spline.commands();
  const auto& points = spline.points();

  for (size_t index = 0; index < commands.size(); ++index) {
    const svg::PathSpline::Command& command = commands[index];
    switch (command.type) {
      case svg::PathSpline::CommandType::MoveTo: {
        if (!current.points.empty()) {
          subpaths.push_back(current);
          current = Subpath();
        }
        current.points.push_back(points[command.pointIndex]);
        current.closed = false;
        break;
      }
      case svg::PathSpline::CommandType::LineTo: {
        current.points.push_back(points[command.pointIndex]);
        break;
      }
      case svg::PathSpline::CommandType::CurveTo: {
        assert(!current.points.empty());
        const Vector2d start = current.points.back();
        AppendFlattenedCubic(start, points[command.pointIndex], points[command.pointIndex + 1],
                             points[command.pointIndex + 2], current.points);
        break;
      }
      case svg::PathSpline::CommandType::ClosePath: {
        current.closed = true;
        current.points.push_back(points[command.pointIndex]);
        subpaths.push_back(current);
        current = Subpath();
        break;
      }
    }
  }

  if (!current.points.empty()) {
    subpaths.push_back(current);
  }

  return subpaths;
}

double MiterLength(const Vector2d& point, const Vector2d& joinPoint, double halfWidth) {
  return (joinPoint - point).length() / halfWidth;
}

void AppendBevel(const Vector2d& point, const Vector2d& normalIn, const Vector2d& normalOut,
                 std::vector<Vector2d>& outline) {
  outline.push_back(point + normalIn);
  outline.push_back(point + normalOut);
}

void AppendRoundJoin(const Vector2d& center, const Vector2d& normalIn, const Vector2d& normalOut,
                     bool clockwise, std::vector<Vector2d>& outline) {
  AppendArcPoints(center, normalIn, normalOut, clockwise, kRoundJoinSegments, outline, true);
}

void AppendJoin(const Vector2d& point, const Vector2d& dirIn, const Vector2d& dirOut,
                const Stroke& stroke, bool leftSide, std::vector<Vector2d>& outline) {
  const Vector2d normalIn =
      leftSide ? LeftNormal(dirIn, stroke.width * 0.5) : RightNormal(dirIn, stroke.width * 0.5);
  const Vector2d normalOut =
      leftSide ? LeftNormal(dirOut, stroke.width * 0.5) : RightNormal(dirOut, stroke.width * 0.5);
  const double turn = Cross(dirIn, dirOut);
  const bool isClockwise = turn < 0.0;

  if (stroke.lineJoin == LineJoin::kRound) {
    AppendRoundJoin(point, normalIn, normalOut, leftSide ? isClockwise : !isClockwise, outline);
    return;
  }

  const std::optional<Vector2d> miter =
      IntersectLines(point + normalIn, dirIn, point + normalOut, dirOut);
  if (stroke.lineJoin == LineJoin::kMiterClip) {
    if (miter.has_value()) {
      const double miterLen = MiterLength(point, *miter, stroke.width * 0.5);
      const double maxLength = std::max(1.0, static_cast<double>(stroke.miterLimit));
      if (miterLen <= maxLength) {
        outline.push_back(*miter);
      } else {
        const Vector2d clipped =
            point + Normalize(*miter - point) * (stroke.width * 0.5 * maxLength);
        outline.push_back(clipped);
      }
      return;
    }
    AppendBevel(point, normalIn, normalOut, outline);
    return;
  }

  if (stroke.lineJoin == LineJoin::kMiter) {
    if (miter.has_value() &&
        MiterLength(point, *miter, stroke.width * 0.5) <= static_cast<double>(stroke.miterLimit)) {
      outline.push_back(*miter);
    } else {
      AppendBevel(point, normalIn, normalOut, outline);
    }
    return;
  }

  AppendBevel(point, normalIn, normalOut, outline);
}

Vector2d CapOffsetPoint(const Vector2d& point, const Vector2d& direction, double halfWidth,
                        bool startCap) {
  if (startCap) {
    return point - Normalize(direction) * halfWidth;
  }
  return point + Normalize(direction) * halfWidth;
}

void AppendCap(const Vector2d& point, const Vector2d& direction, const Stroke& stroke,
               std::vector<Vector2d>& leftOutline, std::vector<Vector2d>& rightOutline,
               bool startCap) {
  const double halfWidth = stroke.width * 0.5;
  const Vector2d normalLeft = LeftNormal(direction, halfWidth);
  const Vector2d normalRight = RightNormal(direction, halfWidth);

  switch (stroke.lineCap) {
    case LineCap::kButt: {
      leftOutline.push_back(point + normalLeft);
      rightOutline.push_back(point + normalRight);
      break;
    }
    case LineCap::kSquare: {
      const Vector2d offsetPoint = CapOffsetPoint(point, direction, halfWidth, startCap);
      leftOutline.push_back(offsetPoint + normalLeft);
      rightOutline.push_back(offsetPoint + normalRight);
      break;
    }
    case LineCap::kRound: {
      leftOutline.push_back(point + normalLeft);
      rightOutline.push_back(point + normalRight);
      break;
    }
  }
}

svg::PathSpline BuildStrokedSubpath(const Subpath& subpath, const Stroke& stroke) {
  svg::PathSpline outline;
  std::vector<Vector2d> points = subpath.points;

  if (subpath.closed && points.size() >= 2) {
    const Vector2d delta = points.front() - points.back();
    if (delta.length() <= kDistanceEpsilon) {
      points.pop_back();
    }
  }

  if (points.size() < 2) {
    return outline;
  }

  const size_t count = points.size();

  std::vector<Vector2d> leftOutline;
  std::vector<Vector2d> rightOutline;

  auto segmentDirection = [&](size_t index) {
    const size_t nextIndex = (index + 1) % count;
    return points[nextIndex] - points[index];
  };

  const Vector2d firstDir = segmentDirection(0);
  const Vector2d lastDir =
      subpath.closed ? segmentDirection(count - 1) : segmentDirection(count - 2);

  if (!subpath.closed) {
    AppendCap(points.front(), firstDir, stroke, leftOutline, rightOutline, true);
  }

  for (size_t i = 0; i < count; ++i) {
    const size_t prevIndex = (i == 0) ? (subpath.closed ? count - 1 : 0) : i - 1;
    const size_t nextIndex = (i + 1) % count;

    const Vector2d point = points[i];
    const Vector2d dirIn = Normalize(points[i] - points[prevIndex]);
    const Vector2d dirOut = Normalize(points[nextIndex] - points[i]);

    if (i == 0 && !subpath.closed) {
      continue;
    }

    if (i == count - 1 && !subpath.closed) {
      continue;
    }

    if (dirIn.length() <= kDistanceEpsilon || dirOut.length() <= kDistanceEpsilon) {
      continue;
    }

    AppendJoin(point, dirIn, dirOut, stroke, true, leftOutline);
    AppendJoin(point, dirIn, dirOut, stroke, false, rightOutline);
  }

  if (!subpath.closed) {
    AppendCap(points.back(), lastDir, stroke, leftOutline, rightOutline, false);
  }

  if (leftOutline.empty() || rightOutline.empty()) {
    return outline;
  }

  std::vector<Vector2d> outlinePoints = leftOutline;

  if (!subpath.closed && stroke.lineCap == LineCap::kRound) {
    const Vector2d endLeft = leftOutline.back();
    const Vector2d endRight = rightOutline.back();
    const Vector2d center = points.back();
    AppendArcPoints(center, endLeft - center, endRight - center, true, kRoundJoinSegments,
                    outlinePoints);
  }

  for (size_t i = rightOutline.size(); i-- > 0;) {
    outlinePoints.push_back(rightOutline[i]);
  }

  if (!subpath.closed && stroke.lineCap == LineCap::kRound) {
    const Vector2d startLeft = leftOutline.front();
    const Vector2d startRight = rightOutline.front();
    const Vector2d center = points.front();
    AppendArcPoints(center, startRight - center, startLeft - center, true, kRoundJoinSegments,
                    outlinePoints);
  }

  outline.moveTo(outlinePoints.front());
  for (size_t i = 1; i < outlinePoints.size(); ++i) {
    outline.lineTo(outlinePoints[i]);
  }
  outline.closePath();
  return outline;
}

}  // namespace

PathIterator::PathIterator(const svg::PathSpline& spline) : spline_(&spline) {}

void PathIterator::reset() {
  commandIndex_ = 0;
}

std::optional<PathSegment> PathIterator::next() {
  if (commandIndex_ >= spline_->commands().size()) {
    return std::nullopt;
  }

  const svg::PathSpline::Command& command = spline_->commands()[commandIndex_++];
  return buildSegment(command);
}

PathSegment PathIterator::buildSegment(const svg::PathSpline::Command& command) const {
  PathSegment segment{};
  segment.isInternalPoint = command.isInternalPoint;

  const auto& points = spline_->points();
  assert(command.pointIndex < points.size());

  switch (command.type) {
    case svg::PathSpline::CommandType::MoveTo: {
      segment.verb = PathVerb::kMove;
      segment.points[0] = ToPoint(points[command.pointIndex]);
      segment.pointCount = 1;
      break;
    }
    case svg::PathSpline::CommandType::LineTo: {
      segment.verb = PathVerb::kLine;
      segment.points[0] = ToPoint(points[command.pointIndex]);
      segment.pointCount = 1;
      break;
    }
    case svg::PathSpline::CommandType::CurveTo: {
      segment.verb = PathVerb::kCubic;
      assert(command.pointIndex + 2 < points.size());
      segment.points[0] = ToPoint(points[command.pointIndex]);
      segment.points[1] = ToPoint(points[command.pointIndex + 1]);
      segment.points[2] = ToPoint(points[command.pointIndex + 2]);
      segment.pointCount = 3;
      break;
    }
    case svg::PathSpline::CommandType::ClosePath: {
      segment.verb = PathVerb::kClose;
      segment.points[0] = ToPoint(points[command.pointIndex]);
      segment.pointCount = 1;
      break;
    }
  }

  return segment;
}

std::optional<Boxd> ComputeBoundingBox(const svg::PathSpline& spline) {
  if (spline.commands().empty()) {
    return std::nullopt;
  }

  const auto& points = spline.points();
  Boxd bounds(
      Vector2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max()),
      Vector2d(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()));

  Vector2d currentPoint{};
  bool hasPoint = false;

  for (const svg::PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case svg::PathSpline::CommandType::MoveTo: {
        assert(command.pointIndex < points.size());
        currentPoint = points[command.pointIndex];
        if (!hasPoint) {
          bounds.topLeft = currentPoint;
          bounds.bottomRight = currentPoint;
          hasPoint = true;
        } else {
          UpdateBoundsWithPoint(currentPoint, bounds);
        }
        break;
      }
      case svg::PathSpline::CommandType::LineTo: {
        assert(command.pointIndex < points.size());
        const Vector2d target = points[command.pointIndex];
        if (!hasPoint) {
          bounds.topLeft = currentPoint;
          bounds.bottomRight = currentPoint;
          hasPoint = true;
        }
        UpdateBoundsWithPoint(currentPoint, bounds);
        UpdateBoundsWithPoint(target, bounds);
        currentPoint = target;
        break;
      }
      case svg::PathSpline::CommandType::CurveTo: {
        assert(command.pointIndex + 2 < points.size());
        const Vector2d control1 = points[command.pointIndex];
        const Vector2d control2 = points[command.pointIndex + 1];
        const Vector2d endPoint = points[command.pointIndex + 2];
        if (!hasPoint) {
          bounds.topLeft = currentPoint;
          bounds.bottomRight = currentPoint;
          hasPoint = true;
        }

        UpdateBoundsWithPoint(currentPoint, bounds);
        UpdateBoundsWithPoint(endPoint, bounds);

        UpdateCubicExtrema(currentPoint.x, control1.x, control2.x, endPoint.x, [&](double t) {
          const double x = EvaluateCubic(currentPoint.x, control1.x, control2.x, endPoint.x, t);
          const double y = EvaluateCubic(currentPoint.y, control1.y, control2.y, endPoint.y, t);
          UpdateBoundsWithPoint(Vector2d(x, y), bounds);
        });
        UpdateCubicExtrema(currentPoint.y, control1.y, control2.y, endPoint.y, [&](double t) {
          const double x = EvaluateCubic(currentPoint.x, control1.x, control2.x, endPoint.x, t);
          const double y = EvaluateCubic(currentPoint.y, control1.y, control2.y, endPoint.y, t);
          UpdateBoundsWithPoint(Vector2d(x, y), bounds);
        });

        currentPoint = endPoint;
        break;
      }
      case svg::PathSpline::CommandType::ClosePath: {
        assert(command.pointIndex < points.size());
        const Vector2d target = points[command.pointIndex];
        if (!hasPoint) {
          bounds.topLeft = currentPoint;
          bounds.bottomRight = currentPoint;
          hasPoint = true;
        }
        UpdateBoundsWithPoint(currentPoint, bounds);
        UpdateBoundsWithPoint(target, bounds);
        currentPoint = target;
        break;
      }
    }
  }

  return bounds;
}

svg::PathSpline ApplyDash(const svg::PathSpline& spline, const StrokeDash& dash) {
  svg::PathSpline dashed;
  if (spline.commands().empty()) {
    return dashed;
  }

  const auto& commands = spline.commands();
  const auto& points = spline.points();

  std::vector<Vector2d> polyline;

  for (size_t index = 0; index < commands.size(); ++index) {
    const svg::PathSpline::Command& command = commands[index];
    switch (command.type) {
      case svg::PathSpline::CommandType::MoveTo: {
        if (polyline.size() >= 2) {
          DashPolyline(polyline, dash, dashed);
        }
        polyline.clear();
        polyline.push_back(points[command.pointIndex]);
        break;
      }
      case svg::PathSpline::CommandType::LineTo: {
        polyline.push_back(points[command.pointIndex]);
        break;
      }
      case svg::PathSpline::CommandType::CurveTo: {
        assert(!polyline.empty());
        const Vector2d start = polyline.back();
        AppendFlattenedCubic(start, points[command.pointIndex], points[command.pointIndex + 1],
                             points[command.pointIndex + 2], polyline);
        break;
      }
      case svg::PathSpline::CommandType::ClosePath: {
        polyline.push_back(points[command.pointIndex]);
        if (polyline.size() >= 2) {
          DashPolyline(polyline, dash, dashed);
        }
        polyline.clear();
        break;
      }
    }
  }

  if (polyline.size() >= 2) {
    DashPolyline(polyline, dash, dashed);
  }

  return dashed;
}

svg::PathSpline ApplyStroke(const svg::PathSpline& spline, const Stroke& stroke) {
  const bool hasDash = stroke.dash.has_value();
  const svg::PathSpline& source = hasDash ? ApplyDash(spline, *stroke.dash) : spline;

  svg::PathSpline stroked;
  const std::vector<Subpath> subpaths = FlattenSpline(source);
  for (const Subpath& subpath : subpaths) {
    svg::PathSpline outline = BuildStrokedSubpath(subpath, stroke);
    PathIterator iterator(outline);
    for (std::optional<PathSegment> segment = iterator.next(); segment.has_value();
         segment = iterator.next()) {
      switch (segment->verb) {
        case PathVerb::kMove: stroked.moveTo(FromPoint(segment->points[0])); break;
        case PathVerb::kLine: stroked.lineTo(FromPoint(segment->points[0])); break;
        case PathVerb::kCubic:
          stroked.curveTo(FromPoint(segment->points[0]), FromPoint(segment->points[1]),
                          FromPoint(segment->points[2]));
          break;
        case PathVerb::kClose: stroked.closePath(); break;
      }
    }
  }

  return stroked;
}

std::optional<Boxd> ComputeStrokeBounds(const svg::PathSpline& spline, const Stroke& stroke) {
  if (spline.commands().empty()) {
    return std::nullopt;
  }

  const svg::PathSpline outline = ApplyStroke(spline, stroke);
  return ComputeBoundingBox(outline);
}

}  // namespace donner::backends::tiny_skia_cpp
