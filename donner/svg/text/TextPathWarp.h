#pragma once
/// @file

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg {

inline constexpr size_t kMaximumTextPathWarpExpansion = 64u;

namespace text_path_warp_details {

inline bool InvalidSample(double distance, double length) {
  return !std::isfinite(distance) || !(length > 0.0) || !std::isfinite(length);
}

inline bool FiniteOutline(const Path& outline) {
  return std::all_of(outline.points().begin(), outline.points().end(), [](const Vector2d& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
  });
}

inline bool CrossesBoundary(double start, double end, double boundary) {
  return start < end ? boundary > start && boundary < end : boundary < start && boundary > end;
}

inline Path::PointOnPath Sample(const StretchedTextPath& mapping, double distance) {
  const double length = mapping.measuredPath.pathLength();
  if (InvalidSample(distance, length)) {
    return {.valid = false};
  }
  if (mapping.closed) {
    const double normalized = std::fmod(distance, length);
    return mapping.measuredPath.pointAtArcLength(normalized < 0.0 ? normalized + length
                                                                  : normalized);
  }
  if (distance >= 0.0 && distance <= length) {
    return mapping.measuredPath.pointAtArcLength(distance);
  }
  const double endpointDistance = distance < 0.0 ? 0.0 : length;
  Path::PointOnPath endpoint = mapping.measuredPath.pointAtArcLength(endpointDistance);
  if (!endpoint.valid || !std::isfinite(endpoint.angle)) {
    return {.valid = false};
  }
  const double extension = distance - endpointDistance;
  endpoint.point += Vector2d(std::cos(endpoint.angle), std::sin(endpoint.angle)) * extension;
  endpoint.valid = std::isfinite(endpoint.point.x) && std::isfinite(endpoint.point.y);
  return endpoint;
}

inline std::optional<Vector2d> Point(const StretchedTextPath& mapping, const TextGlyph& glyph,
                                     const Vector2d& point) {
  if (!glyph.textPathOrigin || !std::isfinite(*glyph.textPathOrigin) ||
      !std::isfinite(glyph.textPathNormalOffset) || !std::isfinite(point.x) ||
      !std::isfinite(point.y)) {
    return std::nullopt;
  }
  const auto sample = Sample(mapping, mapping.startOffset + *glyph.textPathOrigin + point.x);
  const double normal = glyph.textPathNormalOffset + point.y;
  if (!sample.valid || !std::isfinite(sample.angle) || !std::isfinite(normal)) {
    return std::nullopt;
  }
  const Vector2d result(sample.point.x - std::sin(sample.angle) * normal,
                        sample.point.y + std::cos(sample.angle) * normal);
  return std::isfinite(result.x) && std::isfinite(result.y) ? std::optional(result) : std::nullopt;
}

inline bool AppendTransformed(PathBuilder& builder, const Path::Command& command,
                              std::span<const Vector2d> points, const Transform2d& transform) {
  switch (command.verb) {
    case Path::Verb::MoveTo:
      builder.moveTo(transform.transformPosition(points[command.pointIndex]));
      return true;
    case Path::Verb::LineTo:
      builder.lineTo(transform.transformPosition(points[command.pointIndex]));
      return true;
    case Path::Verb::QuadTo:
      builder.quadTo(transform.transformPosition(points[command.pointIndex]),
                     transform.transformPosition(points[command.pointIndex + 1]));
      return true;
    case Path::Verb::CurveTo:
      builder.curveTo(transform.transformPosition(points[command.pointIndex]),
                      transform.transformPosition(points[command.pointIndex + 1]),
                      transform.transformPosition(points[command.pointIndex + 2]));
      return true;
    case Path::Verb::ClosePath: builder.closePath(); return true;
  }
  return false;
}

inline Path LinearOutline(const Path& outline, const StretchedTextPath& mapping,
                          const TextGlyph& glyph) {
  const auto sample = Sample(mapping, mapping.startOffset + *glyph.textPathOrigin);
  if (!sample.valid || !std::isfinite(sample.angle) || !std::isfinite(glyph.textPathNormalOffset)) {
    return Path();
  }
  const Vector2d origin(sample.point.x - std::sin(sample.angle) * glyph.textPathNormalOffset,
                        sample.point.y + std::cos(sample.angle) * glyph.textPathNormalOffset);
  Transform2d transform = Transform2d::Translate(origin);
  if (sample.angle != 0.0) {
    transform = Transform2d::Rotate(sample.angle) * transform;
  }
  PathBuilder builder;
  for (const auto& command : outline.commands()) {
    if (!AppendTransformed(builder, command, outline.points(), transform)) {
      return Path();
    }
  }
  return builder.build();
}

class GeneralOutlineWarper {
public:
  GeneralOutlineWarper(const StretchedTextPath& mapping, const TextGlyph& glyph,
                       std::span<const Vector2d> points, double tolerance)
      : mapping_(mapping),
        glyph_(glyph),
        points_(points),
        tolerance_(tolerance),
        remainingOutput_(points.size() * kMaximumTextPathWarpExpansion),
        remainingBoundaryStorage_(points.size() * kMaximumTextPathWarpExpansion) {}

  bool append(const Path::Command& command) {
    switch (command.verb) {
      case Path::Verb::MoveTo: return moveTo(command.pointIndex);
      case Path::Verb::LineTo: return lineTo(command.pointIndex);
      case Path::Verb::QuadTo: return quadTo(command.pointIndex);
      case Path::Verb::CurveTo: return curveTo(command.pointIndex);
      case Path::Verb::ClosePath: return close();
    }
    return false;
  }
  Path build() { return builder_.build(); }

private:
  std::optional<Vector2d> map(const Vector2d& point) const {
    return Point(mapping_, glyph_, point);
  }
  bool moveTo(size_t index) {
    current_ = points_[index];
    subpathStart_ = current_;
    hasCurrent_ = true;
    const auto mapped = map(current_);
    if (!mapped) {
      return false;
    }
    builder_.moveTo(*mapped);
    return true;
  }
  bool lineTo(size_t index) { return appendLine(points_[index]); }
  bool quadTo(size_t index) {
    return appendQuadratic(current_, points_[index], points_[index + 1], 0u);
  }
  bool curveTo(size_t index) {
    return appendCubic(current_, points_[index], points_[index + 1], points_[index + 2], 0u);
  }
  bool close() {
    if (!appendLine(subpathStart_)) {
      return false;
    }
    builder_.closePath();
    return true;
  }
  bool appendLine(const Vector2d& end) {
    if (!hasCurrent_) {
      return false;
    }
    Vector2d intervalStart = current_;
    auto mappedStart = map(intervalStart);
    if (!mappedStart) {
      return false;
    }
    const double origin = mapping_.startOffset + *glyph_.textPathOrigin;
    const double startDistance = origin + current_.x;
    const double endDistance = origin + end.x;
    const auto boundaries = crossedBoundaries(startDistance, endDistance);
    if (!boundaries) {
      return false;
    }
    if (end.x != current_.x) {
      for (const double boundary : *boundaries) {
        const double t = (boundary - startDistance) / (endDistance - startDistance);
        const Vector2d intervalEnd = current_ * (1.0 - t) + end * t;
        const auto mappedEnd = map(intervalEnd);
        if (!mappedEnd ||
            !appendAdaptive(intervalStart, intervalEnd, *mappedStart, *mappedEnd, 0u)) {
          return false;
        }
        intervalStart = intervalEnd;
        mappedStart = mappedEnd;
      }
    }
    const auto mappedEnd = map(end);
    if (!mappedEnd || !appendAdaptive(intervalStart, end, *mappedStart, *mappedEnd, 0u)) {
      return false;
    }
    current_ = end;
    return true;
  }
  std::optional<std::vector<double>> crossedBoundaries(double start, double end) {
    std::vector<double> result;
    if (start == end || mapping_.segmentEndDistances.empty()) {
      return result;
    }
    const double low = std::min(start, end);
    const double high = std::max(start, end);
    const bool admitted = mapping_.closed ? appendClosedBoundaries(result, low, high)
                                          : appendBoundaryRange(result, low, high, 0.0);
    if (!admitted) {
      return std::nullopt;
    }
    if (start > end) {
      std::reverse(result.begin(), result.end());
    }
    return result;
  }
  bool appendBoundaryRange(std::vector<double>& result, double low, double high, double shift) {
    const auto first = std::upper_bound(mapping_.segmentEndDistances.begin(),
                                        mapping_.segmentEndDistances.end(), low - shift);
    const auto last = std::lower_bound(first, mapping_.segmentEndDistances.end(), high - shift);
    for (auto it = first; it != last; ++it) {
      if (remainingWork_ == 0u || remainingBoundaryStorage_ == 0u) {
        return false;
      }
      --remainingWork_;
      --remainingBoundaryStorage_;
      result.push_back(*it + shift);
    }
    return true;
  }
  bool appendClosedBoundaries(std::vector<double>& result, double low, double high) {
    const double length = mapping_.measuredPath.pathLength();
    const double firstPeriod = std::floor(low / length);
    const double lastPeriod = std::floor(high / length);
    constexpr double kMaxCircuit = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    constexpr double kMinCircuit = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    if (!std::isfinite(firstPeriod) || !std::isfinite(lastPeriod) || firstPeriod < kMinCircuit ||
        lastPeriod >= kMaxCircuit ||
        lastPeriod - firstPeriod > static_cast<double>(remainingWork_)) {
      return false;
    }
    const std::int64_t firstCircuit = static_cast<std::int64_t>(firstPeriod);
    const std::int64_t lastCircuit = static_cast<std::int64_t>(lastPeriod);
    if (lastCircuit < firstCircuit) {
      return false;
    }
    for (std::int64_t circuit = firstCircuit;; ++circuit) {
      if (remainingWork_ == 0u) {
        return false;
      }
      --remainingWork_;
      if (!appendBoundaryRange(result, low, high, static_cast<double>(circuit) * length)) {
        return false;
      }
      if (circuit == lastCircuit) {
        break;
      }
    }
    return true;
  }
  bool appendAdaptive(const Vector2d& start, const Vector2d& end, const Vector2d& mappedStart,
                      const Vector2d& mappedEnd, size_t depth) {
    if (remainingWork_ < 3u || depth > 16u) {
      return false;
    }
    remainingWork_ -= 3u;
    const Vector2d quarter = start * 0.75 + end * 0.25;
    const Vector2d middle = (start + end) * 0.5;
    const Vector2d threeQuarter = start * 0.25 + end * 0.75;
    const auto mappedQuarter = map(quarter);
    const auto mappedMiddle = map(middle);
    const auto mappedThreeQuarter = map(threeQuarter);
    if (!mappedQuarter || !mappedMiddle || !mappedThreeQuarter) {
      return false;
    }
    const Vector2d chordQuarter = mappedStart * 0.75 + mappedEnd * 0.25;
    const Vector2d chordMiddle = (mappedStart + mappedEnd) * 0.5;
    const Vector2d chordThreeQuarter = mappedStart * 0.25 + mappedEnd * 0.75;
    if ((*mappedQuarter - chordQuarter).length() <= tolerance_ &&
        (*mappedMiddle - chordMiddle).length() <= tolerance_ &&
        (*mappedThreeQuarter - chordThreeQuarter).length() <= tolerance_) {
      if (remainingOutput_ == 0u) {
        return false;
      }
      --remainingOutput_;
      builder_.lineTo(mappedEnd);
      return true;
    }
    return appendAdaptive(start, middle, mappedStart, *mappedMiddle, depth + 1u) &&
           appendAdaptive(middle, end, *mappedMiddle, mappedEnd, depth + 1u);
  }
  bool appendQuadratic(const Vector2d& start, const Vector2d& control, const Vector2d& end,
                       size_t depth) {
    if (remainingWork_ < 3u || depth > 16u) {
      return false;
    }
    remainingWork_ -= 3u;
    const auto evaluate = [&](double t) {
      const double u = 1.0 - t;
      return start * (u * u) + control * (2.0 * u * t) + end * (t * t);
    };
    if (mappedCurveWithinTolerance(start, end, evaluate(0.25), evaluate(0.5), evaluate(0.75))) {
      return appendLine(end);
    }
    const Vector2d first = (start + control) * 0.5;
    const Vector2d second = (control + end) * 0.5;
    const Vector2d middle = (first + second) * 0.5;
    return appendQuadratic(start, first, middle, depth + 1u) &&
           appendQuadratic(middle, second, end, depth + 1u);
  }
  bool appendCubic(const Vector2d& start, const Vector2d& firstControl,
                   const Vector2d& secondControl, const Vector2d& end, size_t depth) {
    if (remainingWork_ < 3u || depth > 16u) {
      return false;
    }
    remainingWork_ -= 3u;
    const auto evaluate = [&](double t) {
      const double u = 1.0 - t;
      return start * (u * u * u) + firstControl * (3.0 * u * u * t) +
             secondControl * (3.0 * u * t * t) + end * (t * t * t);
    };
    if (mappedCurveWithinTolerance(start, end, evaluate(0.25), evaluate(0.5), evaluate(0.75))) {
      return appendLine(end);
    }
    const Vector2d a = (start + firstControl) * 0.5;
    const Vector2d b = (firstControl + secondControl) * 0.5;
    const Vector2d c = (secondControl + end) * 0.5;
    const Vector2d d = (a + b) * 0.5;
    const Vector2d e = (b + c) * 0.5;
    const Vector2d middle = (d + e) * 0.5;
    return appendCubic(start, a, d, middle, depth + 1u) &&
           appendCubic(middle, e, c, end, depth + 1u);
  }
  bool mappedCurveWithinTolerance(const Vector2d& start, const Vector2d& end,
                                  const Vector2d& quarter, const Vector2d& middle,
                                  const Vector2d& threeQuarter) const {
    const auto mappedStart = map(start);
    const auto mappedEnd = map(end);
    const auto mappedQuarter = map(quarter);
    const auto mappedMiddle = map(middle);
    const auto mappedThreeQuarter = map(threeQuarter);
    if (!mappedStart || !mappedEnd || !mappedQuarter || !mappedMiddle || !mappedThreeQuarter) {
      return false;
    }
    return (*mappedQuarter - (*mappedStart * 0.75 + *mappedEnd * 0.25)).length() <= tolerance_ &&
           (*mappedMiddle - (*mappedStart + *mappedEnd) * 0.5).length() <= tolerance_ &&
           (*mappedThreeQuarter - (*mappedStart * 0.25 + *mappedEnd * 0.75)).length() <= tolerance_;
  }
  const StretchedTextPath& mapping_;
  const TextGlyph& glyph_;
  std::span<const Vector2d> points_;
  PathBuilder builder_;
  Vector2d current_;
  Vector2d subpathStart_;
  bool hasCurrent_ = false;
  double tolerance_;
  size_t remainingWork_ = Path::kMaximumGeometryQueryWork;
  size_t remainingOutput_ = 0u;
  size_t remainingBoundaryStorage_ = 0u;
};

inline Path GeneralOutline(const Path& outline, const StretchedTextPath& mapping,
                           const TextGlyph& glyph, double tolerance) {
  GeneralOutlineWarper warper(mapping, glyph, outline.points(), tolerance);
  for (const auto& command : outline.commands()) {
    if (!warper.append(command)) {
      return Path();
    }
  }
  return warper.build();
}

}  // namespace text_path_warp_details

/// Map one glyph-local point through a stretched text-path placement.
inline std::optional<Vector2d> MapTextPathPoint(const StretchedTextPath& mapping,
                                                const TextGlyph& glyph, const Vector2d& point) {
  return text_path_warp_details::Point(mapping, glyph, point);
}

/// Warp glyph outline endpoints and control points along text-path perpendiculars.
inline Path WarpTextPathOutline(const Path& outline, const StretchedTextPath& mapping,
                                const TextGlyph& glyph,
                                double tolerance = Path::kLocalFlattenTolerance) {
  if (outline.empty() || !text_path_warp_details::FiniteOutline(outline) ||
      outline.verbCount() > Path::kMaximumGeometryQueryWork / kMaximumTextPathWarpExpansion ||
      outline.points().size() > Path::kMaximumGeometryQueryWork / kMaximumTextPathWarpExpansion) {
    return Path();
  }
  return mapping.linear && glyph.textPathOrigin
             ? text_path_warp_details::LinearOutline(outline, mapping, glyph)
             : text_path_warp_details::GeneralOutline(outline, mapping, glyph, tolerance);
}

}  // namespace donner::svg
