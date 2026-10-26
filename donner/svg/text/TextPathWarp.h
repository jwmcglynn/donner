#pragma once
/// @file

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "donner/base/BezierUtils.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg {

inline constexpr size_t kMaximumTextPathWarpExpansion = 256u;
inline constexpr double kDefaultTextPathWarpTolerance = 0.1;

namespace text_path_warp_details {

inline bool InvalidSample(double distance, double length) {
  return !std::isfinite(distance) || !(length > 0.0) || !std::isfinite(length);
}

inline bool FiniteOutline(const Path& outline) {
  return std::all_of(outline.points().begin(), outline.points().end(), [](const Vector2d& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
  });
}

inline bool ValidMappedPointInput(const TextGlyph& glyph, const Vector2d& point) {
  return glyph.textPathOrigin && std::isfinite(*glyph.textPathOrigin) &&
         std::isfinite(glyph.textPathNormalOffset) && std::isfinite(point.x) &&
         std::isfinite(point.y);
}

inline bool CrossesBoundary(double start, double end, double boundary) {
  return start < end ? boundary > start && boundary < end : boundary < start && boundary > end;
}

inline Path::PointOnPath SampleClosed(const StretchedTextPath& mapping, double distance,
                                      double length,
                                      Path::MeasuredPath::BoundarySide boundarySide) {
  const double normalized = std::fmod(distance, length);
  double sampledDistance = normalized < 0.0 ? normalized + length : normalized;
  if (sampledDistance == 0.0 && boundarySide == Path::MeasuredPath::BoundarySide::Incoming) {
    sampledDistance = length;
  }
  return mapping.measuredPath.pointAtArcLength(sampledDistance, boundarySide);
}

inline Path::PointOnPath SampleOpen(const StretchedTextPath& mapping, double distance,
                                    double length, Path::MeasuredPath::BoundarySide boundarySide) {
  if (distance >= 0.0 && distance <= length) {
    // An open start has no incoming side; the outgoing side skips leading zero-length segments.
    return mapping.measuredPath.pointAtArcLength(
        distance, distance == 0.0 ? Path::MeasuredPath::BoundarySide::Outgoing : boundarySide);
  }
  const bool beforeStart = distance < 0.0;
  const double endpointDistance = beforeStart ? 0.0 : length;
  // Sample the side facing the extension: the outgoing side skips a leading zero-length segment,
  // and the incoming side at the end already resolves to the last nonzero segment.
  Path::PointOnPath endpoint = mapping.measuredPath.pointAtArcLength(
      endpointDistance, beforeStart ? Path::MeasuredPath::BoundarySide::Outgoing
                                    : Path::MeasuredPath::BoundarySide::Incoming);
  if (!endpoint.valid || !std::isfinite(endpoint.angle)) {
    return {.valid = false};
  }
  const double extension = distance - endpointDistance;
  endpoint.point += Vector2d(std::cos(endpoint.angle), std::sin(endpoint.angle)) * extension;
  endpoint.valid = std::isfinite(endpoint.point.x) && std::isfinite(endpoint.point.y);
  return endpoint;
}

inline Path::PointOnPath Sample(
    const StretchedTextPath& mapping, double distance,
    Path::MeasuredPath::BoundarySide boundarySide = Path::MeasuredPath::BoundarySide::Incoming) {
  const double length = mapping.measuredPath.pathLength();
  if (InvalidSample(distance, length)) {
    return {.valid = false};
  }
  return mapping.closed ? SampleClosed(mapping, distance, length, boundarySide)
                        : SampleOpen(mapping, distance, length, boundarySide);
}

inline std::optional<Vector2d> PointAtDistance(
    const StretchedTextPath& mapping, const TextGlyph& glyph, const Vector2d& point,
    double distance,
    Path::MeasuredPath::BoundarySide boundarySide = Path::MeasuredPath::BoundarySide::Incoming) {
  if (!ValidMappedPointInput(glyph, point) || !std::isfinite(distance)) {
    return std::nullopt;
  }
  const auto sample = Sample(mapping, distance, boundarySide);
  const double normal = glyph.textPathNormalOffset + point.y;
  if (!sample.valid || !std::isfinite(sample.angle) || !std::isfinite(normal)) {
    return std::nullopt;
  }
  const Vector2d result(sample.point.x - std::sin(sample.angle) * normal,
                        sample.point.y + std::cos(sample.angle) * normal);
  return std::isfinite(result.x) && std::isfinite(result.y) ? std::optional(result) : std::nullopt;
}

inline std::optional<Vector2d> Point(
    const StretchedTextPath& mapping, const TextGlyph& glyph, const Vector2d& point,
    Path::MeasuredPath::BoundarySide boundarySide = Path::MeasuredPath::BoundarySide::Incoming) {
  if (!glyph.textPathOrigin || !std::isfinite(*glyph.textPathOrigin)) {
    return std::nullopt;
  }
  return PointAtDistance(mapping, glyph, point,
                         mapping.startOffset + *glyph.textPathOrigin + point.x, boundarySide);
}

inline bool AppendTransformed(PathBuilder& builder, const Path::Command& command,
                              std::span<const Vector2d> points,
                              const Transform2d& textElementFromGlyphLocal) {
  switch (command.verb) {
    case Path::Verb::MoveTo:
      builder.moveTo(textElementFromGlyphLocal.transformPosition(points[command.pointIndex]));
      return true;
    case Path::Verb::LineTo:
      builder.lineTo(textElementFromGlyphLocal.transformPosition(points[command.pointIndex]));
      return true;
    case Path::Verb::QuadTo:
      builder.quadTo(textElementFromGlyphLocal.transformPosition(points[command.pointIndex]),
                     textElementFromGlyphLocal.transformPosition(points[command.pointIndex + 1]));
      return true;
    case Path::Verb::CurveTo:
      builder.curveTo(textElementFromGlyphLocal.transformPosition(points[command.pointIndex]),
                      textElementFromGlyphLocal.transformPosition(points[command.pointIndex + 1]),
                      textElementFromGlyphLocal.transformPosition(points[command.pointIndex + 2]));
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
  Transform2d textElementFromGlyphLocal = Transform2d::Translate(origin);
  if (sample.angle != 0.0) {
    textElementFromGlyphLocal = Transform2d::Rotate(sample.angle) * textElementFromGlyphLocal;
  }
  PathBuilder builder;
  for (const auto& command : outline.commands()) {
    if (!AppendTransformed(builder, command, outline.points(), textElementFromGlyphLocal)) {
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
        tolerance_(tolerance * 0.5),
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
  std::optional<Vector2d> map(const Vector2d& point,
                              Path::MeasuredPath::BoundarySide boundarySide) const {
    return Point(mapping_, glyph_, point, boundarySide);
  }
  std::optional<Vector2d> mapBoundary(const Vector2d& point, double sampleDistance,
                                      Path::MeasuredPath::BoundarySide boundarySide) const {
    return PointAtDistance(mapping_, glyph_, point, sampleDistance, boundarySide);
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
    mappedCurrent_ = mapped;
    return true;
  }
  bool lineTo(size_t index) { return appendLine(points_[index]); }
  bool quadTo(size_t index) {
    return appendQuadraticAtExtrema(current_, points_[index], points_[index + 1]);
  }
  bool curveTo(size_t index) {
    return appendCubicAtExtrema(current_, points_[index], points_[index + 1], points_[index + 2]);
  }
  bool close() {
    if (!appendLine(subpathStart_)) {
      return false;
    }
    builder_.closePath();
    return true;
  }
  struct LineState {
    Vector2d intervalStart;
    std::optional<Vector2d> mappedStart;
    double startDistance = 0.0;
    double endDistance = 0.0;
    bool increasing = false;
    Path::MeasuredPath::BoundarySide arrivalSide = Path::MeasuredPath::BoundarySide::Incoming;
  };
  struct Boundary {
    double traversalDistance = 0.0;
    double sampleDistance = 0.0;
  };

  std::optional<LineState> beginLine(const Vector2d& end) {
    if (!hasCurrent_) {
      return std::nullopt;
    }
    const double origin = mapping_.startOffset + *glyph_.textPathOrigin;
    const double startDistance = origin + current_.x;
    const double endDistance = origin + end.x;
    const bool increasing = endDistance > startDistance;
    const bool constantDistance = endDistance == startDistance;
    const auto departureSide = increasing ? Path::MeasuredPath::BoundarySide::Outgoing
                                          : Path::MeasuredPath::BoundarySide::Incoming;
    const auto arrivalSide = constantDistance
                                 ? departureSide
                                 : (increasing ? Path::MeasuredPath::BoundarySide::Incoming
                                               : Path::MeasuredPath::BoundarySide::Outgoing);
    auto mappedStart = isBoundary(startDistance) ? map(current_, departureSide) : map(current_);
    if (!mappedStart || !mappedCurrent_ || !appendBoundaryJoin(*mappedCurrent_, *mappedStart)) {
      return std::nullopt;
    }
    return LineState{current_, mappedStart, startDistance, endDistance, increasing, arrivalSide};
  }

  bool appendLine(const Vector2d& end) {
    auto line = beginLine(end);
    if (!line) {
      return false;
    }
    const auto boundaries = crossedBoundaries(line->startDistance, line->endDistance);
    if (!boundaries) {
      return false;
    }
    bool intervalStartAtBoundary = isBoundary(line->startDistance);
    if (!appendBoundaryIntervals(end, line->startDistance, line->endDistance, line->increasing,
                                 *boundaries, line->intervalStart, line->mappedStart,
                                 intervalStartAtBoundary)) {
      return false;
    }
    const bool endAtBoundary = isBoundary(line->endDistance);
    const auto mappedEnd = endAtBoundary ? map(end, line->arrivalSide) : map(end);
    if (!mappedEnd || !appendAdaptive(line->intervalStart, end, *line->mappedStart, *mappedEnd, 0u,
                                      intervalStartAtBoundary, endAtBoundary)) {
      return false;
    }
    current_ = end;
    mappedCurrent_ = mappedEnd;
    return true;
  }
  bool appendBoundaryIntervals(const Vector2d& end, double startDistance, double endDistance,
                               bool increasing, std::span<const Boundary> boundaries,
                               Vector2d& intervalStart, std::optional<Vector2d>& mappedStart,
                               bool& intervalStartAtBoundary) {
    if (end.x == current_.x) {
      return true;
    }
    const auto beforeBoundary = increasing ? Path::MeasuredPath::BoundarySide::Incoming
                                           : Path::MeasuredPath::BoundarySide::Outgoing;
    const auto afterBoundary = increasing ? Path::MeasuredPath::BoundarySide::Outgoing
                                          : Path::MeasuredPath::BoundarySide::Incoming;
    for (const Boundary& boundary : boundaries) {
      const double t = (boundary.traversalDistance - startDistance) / (endDistance - startDistance);
      const Vector2d intervalEnd = current_ * (1.0 - t) + end * t;
      const auto mappedEnd = mapBoundary(intervalEnd, boundary.sampleDistance, beforeBoundary);
      if (!mappedEnd || !appendAdaptive(intervalStart, intervalEnd, *mappedStart, *mappedEnd, 0u,
                                        intervalStartAtBoundary, /*endAtBoundary=*/true)) {
        return false;
      }
      const auto mappedAfterBoundary =
          mapBoundary(intervalEnd, boundary.sampleDistance, afterBoundary);
      if (!mappedAfterBoundary || !appendBoundaryJoin(*mappedEnd, *mappedAfterBoundary)) {
        return false;
      }
      intervalStart = intervalEnd;
      mappedStart = mappedAfterBoundary;
      intervalStartAtBoundary = true;
    }
    return true;
  }
  bool isBoundary(double distance) const {
    const double length = mapping_.measuredPath.pathLength();
    if (!std::isfinite(distance) || !(length > 0.0)) {
      return false;
    }
    double candidate = distance;
    if (mapping_.closed) {
      candidate = std::fmod(distance, length);
      if (candidate < 0.0) {
        candidate += length;
      }
      if (candidate == 0.0) {
        return true;
      }
    }
    return std::binary_search(mapping_.segmentEndDistances.begin(),
                              mapping_.segmentEndDistances.end(), candidate);
  }
  bool appendBoundaryJoin(const Vector2d& incoming, const Vector2d& outgoing) {
    if (incoming == outgoing) {
      return true;
    }
    if (remainingWork_ == 0u || remainingOutput_ == 0u) {
      return false;
    }
    --remainingWork_;
    --remainingOutput_;
    builder_.lineTo(outgoing);
    return true;
  }
  std::optional<std::vector<Boundary>> crossedBoundaries(double start, double end) {
    std::vector<Boundary> result;
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
  bool appendBoundaryRange(std::vector<Boundary>& result, double low, double high, double shift) {
    const auto first = std::upper_bound(mapping_.segmentEndDistances.begin(),
                                        mapping_.segmentEndDistances.end(), low - shift);
    const auto last = std::lower_bound(first, mapping_.segmentEndDistances.end(), high - shift);
    for (auto it = first; it != last; ++it) {
      if (remainingWork_ == 0u) {
        return false;
      }
      --remainingWork_;
      // Zero-length segments repeat their end distance, and on a closed path a leading one ends at
      // the seam the previous circuit's final entry already crossed. Emit each crossing once: a
      // repeat would map a zero-length interval between the two one-sided normals.
      const double traversalDistance = *it + shift;
      if ((mapping_.closed && *it == 0.0) ||
          (!result.empty() && traversalDistance <= result.back().traversalDistance)) {
        continue;
      }
      if (remainingBoundaryStorage_ == 0u) {
        return false;
      }
      --remainingBoundaryStorage_;
      result.push_back(Boundary{traversalDistance, *it});
    }
    return true;
  }
  bool appendClosedBoundaries(std::vector<Boundary>& result, double low, double high) {
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
                      const Vector2d& mappedEnd, size_t depth, bool startAtBoundary,
                      bool endAtBoundary) {
    if (depth > 16u) {
      // A leaf this deep that has not converged spans a discontinuity in the mapping. Away from
      // the interval's boundary samples that is a cusp inside one segment, whose normal flips;
      // join across it as at a segment boundary. A leaf touching a boundary sample may sit on a
      // subpath break, which is never joined, so it is refused. The flags come from the caller's
      // boundary split, not from comparing recomputed distances.
      return !startAtBoundary && !endAtBoundary && (end - start).length() <= tolerance_ &&
             appendBoundaryJoin(mappedStart, mappedEnd);
    }
    if (remainingWork_ < 3u) {
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
    return appendAdaptive(start, middle, mappedStart, *mappedMiddle, depth + 1u, startAtBoundary,
                          false) &&
           appendAdaptive(middle, end, *mappedMiddle, mappedEnd, depth + 1u, false, endAtBoundary);
  }
  bool appendQuadratic(const Vector2d& start, const Vector2d& control, const Vector2d& end,
                       size_t depth) {
    if (remainingWork_ < 3u || depth > 16u) {
      return false;
    }
    remainingWork_ -= 3u;
    if (controlResidualWithinTolerance(start, control, end)) {
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
    if (controlResidualWithinTolerance(start, firstControl, end) &&
        controlResidualWithinTolerance(start, secondControl, end)) {
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
  bool appendQuadraticAtExtrema(const Vector2d& start, const Vector2d& control,
                                const Vector2d& end) {
    const auto extrema = normalizedQuadraticXExtrema(start, control, end);
    if (!extrema) {
      return false;
    }
    if (extrema->empty()) {
      return appendQuadratic(start, control, end, 0u);
    }
    const double t = extrema->front();
    if (!std::isfinite(t) || t <= 0.0 || t >= 1.0) {
      return false;
    }
    const auto [left, right] = SplitQuadratic(start, control, end, t);
    return appendQuadratic(left[0], left[1], left[2], 0u) &&
           appendQuadratic(right[0], right[1], right[2], 0u);
  }
  bool appendCubicAtExtrema(const Vector2d& start, const Vector2d& first, const Vector2d& second,
                            const Vector2d& end) {
    std::array<Vector2d, 4> remainder = {start, first, second, end};
    double previous = 0.0;
    const auto extrema = normalizedCubicXExtrema(start, first, second, end);
    if (!extrema) {
      return false;
    }
    for (const double root : *extrema) {
      if (!std::isfinite(root) || root <= previous || root >= 1.0) {
        return false;
      }
      const double relative = (root - previous) / (1.0 - previous);
      if (!std::isfinite(relative) || relative <= 0.0 || relative >= 1.0) {
        return false;
      }
      const auto [left, right] =
          SplitCubic(remainder[0], remainder[1], remainder[2], remainder[3], relative);
      if (!appendCubic(left[0], left[1], left[2], left[3], 0u)) {
        return false;
      }
      remainder = right;
      previous = root;
    }
    return appendCubic(remainder[0], remainder[1], remainder[2], remainder[3], 0u);
  }
  static std::optional<SmallVector<double, 1>> normalizedQuadraticXExtrema(const Vector2d& start,
                                                                           const Vector2d& control,
                                                                           const Vector2d& end) {
    const double controlDelta = control.x - start.x;
    const double endDelta = end.x - start.x;
    const double scale = std::max(std::abs(controlDelta), std::abs(endDelta));
    if (!std::isfinite(controlDelta) || !std::isfinite(endDelta) || !std::isfinite(scale)) {
      return std::nullopt;
    }
    if (scale == 0.0) {
      return SmallVector<double, 1>();
    }
    return QuadraticXExtrema(Vector2d(0.0, 0.0), Vector2d(controlDelta / scale, 0.0),
                             Vector2d(endDelta / scale, 0.0));
  }
  static std::optional<SmallVector<double, 2>> normalizedCubicXExtrema(const Vector2d& start,
                                                                       const Vector2d& first,
                                                                       const Vector2d& second,
                                                                       const Vector2d& end) {
    const double firstDelta = first.x - start.x;
    const double secondDelta = second.x - start.x;
    const double endDelta = end.x - start.x;
    const double scale =
        std::max({std::abs(firstDelta), std::abs(secondDelta), std::abs(endDelta)});
    if (!std::isfinite(firstDelta) || !std::isfinite(secondDelta) || !std::isfinite(endDelta) ||
        !std::isfinite(scale)) {
      return std::nullopt;
    }
    if (scale == 0.0) {
      return SmallVector<double, 2>();
    }
    return CubicXExtrema(Vector2d(0.0, 0.0), Vector2d(firstDelta / scale, 0.0),
                         Vector2d(secondDelta / scale, 0.0), Vector2d(endDelta / scale, 0.0));
  }
  bool controlResidualWithinTolerance(const Vector2d& start, const Vector2d& control,
                                      const Vector2d& end) const {
    const double dx = end.x - start.x;
    if (dx == 0.0) {
      return start.y <= end.y ? control.y >= start.y && control.y <= end.y
                              : control.y <= start.y && control.y >= end.y;
    }
    const double ratio = (control.x - start.x) / dx;
    const double residual = control.y - (start.y + ratio * (end.y - start.y));
    return std::isfinite(ratio) && std::isfinite(residual) && std::abs(residual) <= tolerance_;
  }
  const StretchedTextPath& mapping_;
  const TextGlyph& glyph_;
  std::span<const Vector2d> points_;
  PathBuilder builder_;
  Vector2d current_;
  std::optional<Vector2d> mappedCurrent_;
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
                                double tolerance = kDefaultTextPathWarpTolerance) {
  if (outline.empty() || !text_path_warp_details::FiniteOutline(outline) ||
      outline.verbCount() > Path::kMaximumGeometryQueryWork / kMaximumTextPathWarpExpansion ||
      outline.points().size() > Path::kMaximumGeometryQueryWork / kMaximumTextPathWarpExpansion) {
    return Path();
  }
  return mapping.linear && glyph.textPathOrigin
             ? text_path_warp_details::LinearOutline(outline, mapping, glyph)
             : text_path_warp_details::GeneralOutline(outline, mapping, glyph, tolerance);
}

/// Apply glyph-local authored rotation and the shared nonlinear text-path mapping.
inline Path PlaceStretchedTextPathOutline(Path outline, const TextRun& run, const TextGlyph& glyph,
                                          double tolerance = kDefaultTextPathWarpTolerance) {
  if (!run.stretchedTextPath || !glyph.textPathOrigin) {
    return Path();
  }
  const double rotation = glyph.textPathLocalRotateDegrees.value_or(0.0);
  if (rotation != 0.0) {
    outline =
        outline.transformed(Transform2d::Rotate(rotation * MathConstants<double>::kPi / 180.0));
  }
  return WarpTextPathOutline(outline, *run.stretchedTextPath, glyph, tolerance);
}

/// Build and place the full-font glyph cell for a stretched horizontal run.
inline Path PlaceStretchedTextPathCell(const TextRun& run, const TextGlyph& glyph, double emTop,
                                       double emBottom,
                                       double tolerance = kDefaultTextPathWarpTolerance) {
  const double geometricScaleY = glyph.stretchScaleY;
  Path cell = PathBuilder()
                  .addRect(Box2d::FromXYWH(0.0, -emTop * geometricScaleY, glyph.xAdvance,
                                           (emTop + emBottom) * geometricScaleY))
                  .build();
  return PlaceStretchedTextPathOutline(std::move(cell), run, glyph, tolerance);
}

}  // namespace donner::svg
