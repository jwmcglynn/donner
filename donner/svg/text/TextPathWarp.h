#pragma once
/// @file

#include <cmath>
#include <optional>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg {

namespace text_path_warp_details {

inline bool InvalidSample(double distance, double length) {
  return !std::isfinite(distance) || !(length > 0.0) || !std::isfinite(length);
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

}  // namespace text_path_warp_details

/// Warp glyph outline endpoints and control points along text-path perpendiculars.
inline Path WarpTextPathOutline(const Path& outline, const StretchedTextPath& mapping,
                                const TextGlyph& glyph) {
  constexpr size_t kLineSubdivisions = 8;
  if (outline.empty() ||
      outline.verbCount() > Path::kMaximumGeometryQueryWork / kLineSubdivisions) {
    return Path();
  }
  if (mapping.linear && glyph.textPathOrigin) {
    const auto sample =
        text_path_warp_details::Sample(mapping, mapping.startOffset + *glyph.textPathOrigin);
    if (!sample.valid || !std::isfinite(sample.angle) ||
        !std::isfinite(glyph.textPathNormalOffset)) {
      return Path();
    }
    const Vector2d origin(sample.point.x - std::sin(sample.angle) * glyph.textPathNormalOffset,
                          sample.point.y + std::cos(sample.angle) * glyph.textPathNormalOffset);
    Transform2d transform = Transform2d::Translate(origin);
    if (sample.angle != 0.0) {
      transform = Transform2d::Rotate(sample.angle) * transform;
    }
    PathBuilder linearBuilder;
    const auto sourcePoints = outline.points();
    for (const Path::Command& command : outline.commands()) {
      switch (command.verb) {
        case Path::Verb::MoveTo:
          linearBuilder.moveTo(transform.transformPosition(sourcePoints[command.pointIndex]));
          break;
        case Path::Verb::LineTo:
          linearBuilder.lineTo(transform.transformPosition(sourcePoints[command.pointIndex]));
          break;
        case Path::Verb::QuadTo:
          linearBuilder.quadTo(transform.transformPosition(sourcePoints[command.pointIndex]),
                               transform.transformPosition(sourcePoints[command.pointIndex + 1]));
          break;
        case Path::Verb::CurveTo:
          linearBuilder.curveTo(transform.transformPosition(sourcePoints[command.pointIndex]),
                                transform.transformPosition(sourcePoints[command.pointIndex + 1]),
                                transform.transformPosition(sourcePoints[command.pointIndex + 2]));
          break;
        case Path::Verb::ClosePath: linearBuilder.closePath(); break;
      }
    }
    return linearBuilder.build();
  }
  PathBuilder builder;
  Vector2d current;
  Vector2d subpathStart;
  bool hasCurrent = false;
  const auto mapPoint = [&](const Vector2d& point) {
    return text_path_warp_details::Point(mapping, glyph, point);
  };
  const auto appendLine = [&](const Vector2d& end) {
    if (!hasCurrent) {
      return false;
    }
    const size_t subdivisions = mapping.linear ? 1u : kLineSubdivisions;
    for (size_t i = 1; i <= subdivisions; ++i) {
      const double t = static_cast<double>(i) / static_cast<double>(subdivisions);
      const auto mapped = mapPoint(current * (1.0 - t) + end * t);
      if (!mapped) {
        return false;
      }
      builder.lineTo(*mapped);
    }
    current = end;
    return true;
  };

  const auto points = outline.points();
  for (const Path::Command& command : outline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo: {
        current = points[command.pointIndex];
        subpathStart = current;
        hasCurrent = true;
        const auto mapped = mapPoint(current);
        if (!mapped) {
          return Path();
        }
        builder.moveTo(*mapped);
        break;
      }
      case Path::Verb::LineTo:
        if (!appendLine(points[command.pointIndex])) {
          return Path();
        }
        break;
      case Path::Verb::QuadTo: {
        const auto control = mapPoint(points[command.pointIndex]);
        const auto end = mapPoint(points[command.pointIndex + 1]);
        if (!control || !end) {
          return Path();
        }
        builder.quadTo(*control, *end);
        current = points[command.pointIndex + 1];
        break;
      }
      case Path::Verb::CurveTo: {
        const auto first = mapPoint(points[command.pointIndex]);
        const auto second = mapPoint(points[command.pointIndex + 1]);
        const auto end = mapPoint(points[command.pointIndex + 2]);
        if (!first || !second || !end) {
          return Path();
        }
        builder.curveTo(*first, *second, *end);
        current = points[command.pointIndex + 2];
        break;
      }
      case Path::Verb::ClosePath:
        if (!appendLine(subpathStart)) {
          return Path();
        }
        builder.closePath();
        break;
    }
  }
  return builder.build();
}

}  // namespace donner::svg
