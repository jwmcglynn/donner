#include "donner/svg/components/animation/AnimationMotion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

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

std::optional<double> MotionDistance(const AnimateMotionComponent& motion,
                                     const Path::MeasuredPath& measured, double progress) {
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
    points = measured.segmentEndDistances();
    points.insert(points.begin(), 0);
    for (double& point : points) {
      point = measured.pathLength() > 0 ? point / measured.pathLength() : 0;
    }
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

}  // namespace

SampledAnimationMotion SampleAnimationMotion(Registry& registry, Entity entity,
                                             const AnimateMotionComponent& motion, double progress,
                                             double iteration, AnimationResourceBudget& budget) {
  if (!motion.valid) {
    return {};
  }
  bool additive = motion.additive;
  std::optional<Path> path;
  const auto source = MotionPathSource(registry, entity, motion, budget, path);
  if (!path && source) {
    if (source->empty() || source->size() > AnimationResourceBudget::kMaximumPathValueBytes) {
      return {};
    }
    auto parsed = parser::PathParser::Parse(*source);
    if (parsed.hasError()) {
      return {};
    }
    path = std::move(parsed.result());
  } else if (!path) {
    path = ImplicitMotionPath(motion);
    additive =
        additive || (motion.values.empty() && !motion.from && !motion.to && motion.by.has_value());
  }
  if (!path) {
    return {};
  }
  const auto measured = path->measure(budget.remainingGeometryWork());
  if (!budget.reserveGeometryWork(measured.measurementWorkUnits()) || !measured.valid()) {
    return {};
  }
  const auto distance = MotionDistance(motion, measured, progress);
  if (!distance) {
    return {};
  }
  auto sample = measured.pointAtArcLength(*distance);
  if (!sample.valid) {
    return {};
  }
  if (motion.accumulate && iteration > 0) {
    const auto last = measured.pointAtArcLength(measured.pathLength());
    if (!last.valid) {
      return {};
    }
    sample.point += last.point * iteration;
  }
  const auto angle = MotionAngle(motion, sample.angle);
  if (!angle || !std::isfinite(sample.point.x) || !std::isfinite(sample.point.y)) {
    return {};
  }
  std::ostringstream result;
  result.precision(std::numeric_limits<double>::max_digits10);
  result << "translate(" << sample.point.x << ", " << sample.point.y << ")";
  if (*angle != 0) {
    result << " rotate(" << *angle << ")";
  }
  return {result.str(), additive};
}

}  // namespace donner::svg::components
