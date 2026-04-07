#include "donner/base/Path.h"

#include <algorithm>
#include <cmath>

#include "donner/base/BezierUtils.h"
#include "donner/base/MathUtils.h"

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

// ============================================================================
// PathBuilder
// ============================================================================

PathBuilder& PathBuilder::moveTo(const Vector2d& point) {
  path_.commands_.push_back({Path::Verb::MoveTo, static_cast<uint32_t>(path_.points_.size())});
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
  // Arc-to-cubic decomposition: delegate to a temporary PathSpline-style decomposition.
  // For now, approximate with a cubic from current point to end.
  // TODO(geode): Implement proper arc decomposition from PathSpline::arcTo.
  const Vector2d start = currentPoint();

  // Simple arc approximation using cubic Béziers.
  // Full implementation would use the SVG arc parameterization (Appendix F.6).
  // For the initial implementation, we emit a simple cubic to the endpoint.
  // TODO(geode): Implement proper arc decomposition from PathSpline::arcTo.
  const Vector2d normal = Vector2d(-(end.y - start.y), end.x - start.x) * 0.5;
  curveTo(start + normal * (2.0 / 3.0), end + normal * (2.0 / 3.0), end);
  return *this;
}

PathBuilder& PathBuilder::closePath() {
  path_.commands_.push_back({Path::Verb::ClosePath, static_cast<uint32_t>(path_.points_.size())});
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
  hasMoveTo_ = false;
  return result;
}

void PathBuilder::ensureMoveTo() {
  if (!hasMoveTo_) {
    moveTo(lastMoveTo_);
  }
}

}  // namespace donner
