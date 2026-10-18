#include "donner/svg/components/animation/AnimationMotion.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <sstream>
#include <utility>

#include "donner/base/MathUtils.h"
#include "donner/base/Path.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationReference.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/parser/PointsListParser.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {
namespace {

std::string CopyMotionSource(std::string_view source, AnimationResourceBudget& budget) {
  if (source.size() > AnimationResourceBudget::kMaximumPathValueBytes ||
      !budget.reserveGeometryWork(source.size())) {
    return {};
  }
  return std::string(source);
}

std::optional<Path> CopyMotionSpline(const Path& spline, AnimationResourceBudget& budget) {
  const auto bytes = spline.retainedBytes();
  if (!bytes || *bytes > AnimationResourceBudget::kMaximumPathValueBytes ||
      !budget.reserveOutput(*bytes) || !budget.reserveGeometryWork(spline.verbCount())) {
    return std::nullopt;
  }
  return spline;
}

bool HasAnimatedPathData(Registry& registry, Entity target) {
  const auto* animated = registry.try_get<AnimatedValuesComponent>(target);
  return animated && animated->overrides.contains("d");
}

std::optional<std::string> MotionPathSource(Registry& registry, Entity entity,
                                            const AnimateMotionComponent& motion,
                                            AnimationResourceBudget& budget,
                                            std::optional<Path>& suppliedPath) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
  std::size_t visited = 0;
  for (Entity child = tree ? tree->firstChild() : entt::null; child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    if (++visited > AnimationResourceBudget::kMaximumAnimations) {
      return std::string{};
    }
    const auto* reference = registry.try_get<MPathComponent>(child);
    if (!reference || !IsAnimationInSvgNamespace(registry, child)) {
      continue;
    }
    const auto* attributes = registry.try_get<donner::components::AttributesComponent>(child);
    std::optional<RcString> authoredHref =
        attributes ? attributes->getAttribute("href") : std::nullopt;
    if (const auto* values = registry.try_get<AnimatedValuesComponent>(child)) {
      auto it = values->overrides.find("href");
      if (it == values->overrides.end() && !authoredHref) {
        it = values->overrides.find("xlink:href");
      }
      if (it != values->overrides.end()) {
        if (it->second.size() > Reference::kMaximumHrefBytes ||
            !budget.reserveSourceBytes(it->second.size())) {
          return std::string{};
        }
        authoredHref = RcString(it->second);
      }
    }
    if (!authoredHref && attributes) {
      authoredHref = attributes->getAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
    }
    const std::string_view href =
        authoredHref ? std::string_view(*authoredHref)
                     : (attributes ? std::string_view{} : std::string_view(reference->href));
    if (!href.starts_with('#') || !budget.reserveGeometryWork(href.size())) {
      return std::string{};
    }
    const auto target = FindAnimationReference(registry, href.substr(1), budget);
    const auto* type = registry.try_get<ElementTypeComponent>(target);
    if (!type || type->type() != ElementType::Path ||
        !IsAnimationInSvgNamespace(registry, target)) {
      return std::string{};
    }

    const auto data = CascadedAnimationPathData(registry, target, budget);
    if (!data) {
      return std::string{};
    }
    if (const auto* path = registry.try_get<PathComponent>(target);
        path && path->splineOverride && !HasAnimatedPathData(registry, target)) {
      suppliedPath = CopyMotionSpline(*path->splineOverride, budget);
      return suppliedPath ? std::nullopt : std::optional(std::string{});
    }
    if (data) {
      return CopyMotionSource(std::string_view(*data), budget);
    }
    return std::string{};
  }
  return motion.path ? std::optional(CopyMotionSource(*motion.path, budget)) : std::nullopt;
}

std::optional<Vector2d> ParseMotionPoint(std::string_view value) {
  const auto parsed = parser::PointsListParser::Parse(value);
  if (parsed.hasError() || parsed.result().size() != 1) {
    return std::nullopt;
  }
  const auto point = parsed.result().front();
  return std::isfinite(point.x) && std::isfinite(point.y) ? std::optional(point) : std::nullopt;
}

std::optional<Path> ImplicitMotionPath(const AnimateMotionComponent& motion) {
  std::vector<Vector2d> points;
  if (!motion.values.empty()) {
    for (const auto& value : motion.values) {
      auto point = ParseMotionPoint(value);
      if (!point) {
        return std::nullopt;
      }
      points.push_back(*point);
    }
  } else {
    const auto from =
        motion.from ? ParseMotionPoint(*motion.from) : std::optional(Vector2d::Zero());
    const auto target = motion.to ? ParseMotionPoint(*motion.to)
                                  : (motion.by ? ParseMotionPoint(*motion.by) : std::nullopt);
    if (!from || !target) {
      return std::nullopt;
    }
    points = {*from, motion.to ? *target : *from + *target};
  }
  if (points.empty()) {
    return std::nullopt;
  }
  PathBuilder builder;
  builder.moveTo(points.front());
  if (points.size() == 1) {
    builder.lineTo(points.front());
  }
  for (std::size_t i = 1; i < points.size(); ++i) {
    builder.lineTo(points[i]);
  }
  return builder.build();
}

std::optional<std::vector<double>> AuthoredMotionTimingPoints(
    const Path& path, const Path::MeasuredPath& measured,
    std::span<const double> segmentEndDistances) {
  std::vector<double> points;
  points.reserve(segmentEndDistances.size() + 1);
  points.push_back(0);
  std::size_t segmentIndex = 0;
  for (const Path::Command& command : path.commands()) {
    if (command.verb == Path::Verb::MoveTo) {
      continue;
    }
    if (segmentIndex >= segmentEndDistances.size()) {
      return std::nullopt;
    }
    const double endDistance = segmentEndDistances[segmentIndex++];
    if (!command.isInternal) {
      points.push_back(measured.pathLength() > 0 ? endDistance / measured.pathLength() : 0);
    }
  }
  return segmentIndex == segmentEndDistances.size() ? std::optional(std::move(points))
                                                    : std::nullopt;
}

std::optional<double> MotionDistance(const AnimateMotionComponent& motion, const Path& path,
                                     const Path::MeasuredPath& measured,
                                     std::span<const double> segmentEndDistances, double progress) {
  if (motion.calcMode == CalcMode::Paced) {
    return progress * measured.pathLength();
  }
  std::vector<double> points = motion.keyPoints;
  if (!points.empty()) {
    if (points.size() != motion.keyTimes.size() ||
        !std::all_of(points.begin(), points.end(),
                     [](double p) { return std::isfinite(p) && p >= 0 && p <= 1; })) {
      return std::nullopt;
    }
  } else {
    auto authoredPoints = AuthoredMotionTimingPoints(path, measured, segmentEndDistances);
    if (!authoredPoints) {
      return std::nullopt;
    }
    points = std::move(*authoredPoints);
  }
  const auto segment = SelectAnimationSegment(progress, points.size(), motion);
  if (!segment) {
    return std::nullopt;
  }
  return (points[segment->from] +
          (points[segment->to] - points[segment->from]) * segment->progress) *
         measured.pathLength();
}

std::optional<double> MotionAngle(const AnimateMotionComponent& motion, double tangent) {
  if (motion.rotate == "auto") {
    return tangent * MathConstants<double>::kRadToDeg;
  }
  if (motion.rotate == "auto-reverse") {
    return tangent * MathConstants<double>::kRadToDeg + 180;
  }
  const auto number = donner::parser::NumberParser::Parse(motion.rotate);
  if (number.hasError() || number.result().consumedChars != motion.rotate.size() ||
      !std::isfinite(number.result().number)) {
    return std::nullopt;
  }
  return number.result().number;
}

bool IsExactlyZeroVector(Vector2d vector) {
  return vector.x == 0 && vector.y == 0;
}

std::optional<Vector2d> FirstNonzeroVector(Vector2d first, Vector2d second, Vector2d third) {
  for (const Vector2d candidate : {first, second, third}) {
    if (!IsExactlyZeroVector(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<Vector2d> MotionCommandEndpointTangent(const Path& path, std::size_t selectedSegment,
                                                     bool atEnd) {
  const auto commands = path.commands();
  const auto points = path.points();
  Vector2d current;
  Vector2d subpathStart;
  bool hasCurrent = false;
  std::size_t segmentIndex = 0;
  for (const Path::Command& command : commands) {
    if (command.verb == Path::Verb::MoveTo) {
      current = points[command.pointIndex];
      subpathStart = current;
      hasCurrent = true;
      continue;
    }
    if (!hasCurrent) {
      return std::nullopt;
    }

    const Vector2d start = current;
    Vector2d end;
    std::optional<Vector2d> tangent;
    switch (command.verb) {
      case Path::Verb::MoveTo: UTILS_UNREACHABLE();
      case Path::Verb::LineTo:
        end = points[command.pointIndex];
        tangent = end - start;
        break;
      case Path::Verb::QuadTo: {
        const Vector2d control = points[command.pointIndex];
        end = points[command.pointIndex + 1];
        tangent = atEnd ? FirstNonzeroVector(end - control, end - start, {})
                        : FirstNonzeroVector(control - start, end - start, {});
        break;
      }
      case Path::Verb::CurveTo: {
        const Vector2d firstControl = points[command.pointIndex];
        const Vector2d secondControl = points[command.pointIndex + 1];
        end = points[command.pointIndex + 2];
        tangent =
            atEnd ? FirstNonzeroVector(end - secondControl, end - firstControl, end - start)
                  : FirstNonzeroVector(firstControl - start, secondControl - start, end - start);
        break;
      }
      case Path::Verb::ClosePath:
        end = subpathStart;
        tangent = end - start;
        break;
    }
    if (segmentIndex++ == selectedSegment) {
      return tangent;
    }
    current = end;
  }
  return std::nullopt;
}

bool SegmentHasPositiveLength(std::span<const double> segmentEndDistances, std::size_t segment) {
  const double start = segment == 0 ? 0 : segmentEndDistances[segment - 1];
  return segmentEndDistances[segment] > start;
}

bool IsSameMotionDistance(double lhs, double rhs) {
  if (lhs == rhs) {
    return true;
  }
  const double scale = std::max(std::abs(lhs), std::abs(rhs));
  return std::abs(lhs - rhs) <= scale * std::numeric_limits<double>::epsilon() * 8;
}

std::optional<std::pair<std::size_t, bool>> MotionBoundarySegment(
    std::span<const double> segmentEndDistances, double distance) {
  const auto selected =
      std::lower_bound(segmentEndDistances.begin(), segmentEndDistances.end(), distance);
  if (selected == segmentEndDistances.end()) {
    return std::nullopt;
  }
  const std::size_t selectedSegment =
      static_cast<std::size_t>(selected - segmentEndDistances.begin());
  const double startDistance = selectedSegment == 0 ? 0 : segmentEndDistances[selectedSegment - 1];
  if (IsSameMotionDistance(distance, startDistance) && !IsSameMotionDistance(distance, *selected)) {
    return std::pair(selectedSegment, false);
  }
  if (!IsSameMotionDistance(distance, *selected)) {
    return std::nullopt;
  }

  for (std::size_t following = selectedSegment + 1; following < segmentEndDistances.size();
       ++following) {
    if (SegmentHasPositiveLength(segmentEndDistances, following)) {
      return std::pair(following, false);
    }
  }
  for (std::size_t incoming = segmentEndDistances.size(); incoming-- > 0;) {
    if (SegmentHasPositiveLength(segmentEndDistances, incoming)) {
      return std::pair(incoming, true);
    }
  }
  return std::nullopt;
}

std::optional<double> MotionEndpointTangentAngle(const Path& path,
                                                 std::span<const double> segmentEndDistances,
                                                 double distance) {
  const auto selected = MotionBoundarySegment(segmentEndDistances, distance);
  if (!selected) {
    return std::nullopt;
  }
  const auto tangent = MotionCommandEndpointTangent(path, selected->first, selected->second);
  if (!tangent || IsExactlyZeroVector(*tangent)) {
    return std::nullopt;
  }
  return std::atan2(tangent->y, tangent->x);
}

std::optional<Path> ParseMotionPathSource(const std::optional<std::string>& source) {
  if (!source || source->empty() ||
      source->size() > AnimationResourceBudget::kMaximumPathValueBytes) {
    return std::nullopt;
  }
  auto parsed = parser::PathParser::Parse(*source);
  return parsed.hasError() ? std::nullopt : std::optional(std::move(parsed.result()));
}

std::optional<Path> ResolveMotionPath(Registry& registry, Entity entity,
                                      const AnimateMotionComponent& motion,
                                      AnimationResourceBudget& budget, bool& additive) {
  std::optional<Path> path;
  const auto source = MotionPathSource(registry, entity, motion, budget, path);
  if (!path && source) {
    return ParseMotionPathSource(source);
  }
  if (!path) {
    path = ImplicitMotionPath(motion);
    additive =
        additive || (motion.values.empty() && !motion.from && !motion.to && motion.by.has_value());
  }
  return path;
}

bool AccumulateMotion(const AnimateMotionComponent& motion, const Path& path, double iteration,
                      const Path::MeasuredPath& measured,
                      std::span<const double> segmentEndDistances, Path::PointOnPath& sample) {
  if (!motion.accumulate || !(iteration > 0)) {
    return true;
  }
  const auto finalDistance = MotionDistance(motion, path, measured, segmentEndDistances, 1);
  if (!finalDistance) {
    return false;
  }
  const auto last = measured.pointAtArcLength(*finalDistance);
  if (!last.valid) {
    return false;
  }
  sample.point += last.point * iteration;
  return true;
}

std::string FormatMotionTransform(const Vector2d& point, double angle) {
  std::ostringstream result;
  result.precision(std::numeric_limits<double>::max_digits10);
  result << "translate(" << point.x << ", " << point.y << ")";
  if (angle != 0) {
    result << " rotate(" << angle << ")";
  }
  return result.str();
}

}  // namespace

SampledAnimationMotion SampleAnimationMotion(Registry& registry, Entity entity,
                                             const AnimateMotionComponent& motion, double progress,
                                             double iteration, AnimationResourceBudget& budget) {
  if (!motion.valid) {
    return {};
  }
  bool additive = motion.additive;
  const auto path = ResolveMotionPath(registry, entity, motion, budget, additive);
  if (!path) {
    return {};
  }
  const auto measured = path->measure(budget.remainingGeometryWork());
  if (!budget.reserveGeometryWork(measured.measurementWorkUnits()) || !measured.valid()) {
    return {};
  }
  const std::vector<double> segmentEndDistances = measured.segmentEndDistances();
  const auto distance = MotionDistance(motion, *path, measured, segmentEndDistances, progress);
  if (!distance) {
    return {};
  }
  auto sample = measured.pointAtArcLength(*distance);
  if (!sample.valid) {
    return {};
  }
  if (!AccumulateMotion(motion, *path, iteration, measured, segmentEndDistances, sample)) {
    return {};
  }
  double tangentAngle = sample.angle;
  if ((motion.rotate == "auto" || motion.rotate == "auto-reverse") &&
      IsExactlyZeroVector(sample.tangent)) {
    tangentAngle =
        MotionEndpointTangentAngle(*path, segmentEndDistances, *distance).value_or(tangentAngle);
  }
  const auto angle = MotionAngle(motion, tangentAngle);
  if (!angle || !std::isfinite(sample.point.x) || !std::isfinite(sample.point.y)) {
    return {};
  }
  return {FormatMotionTransform(sample.point, *angle), additive};
}

}  // namespace donner::svg::components
