#include "donner/svg/SVGGeometryElement.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg {

double SVGGeometryElement::computedPathLength() const {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (const components::ComputedPathComponent* path =
          components::ShapeSystem().createComputedPathIfShape(handle_, FontMetrics(), disabledSink)) {
    return path->spline.pathLength();
  } else {
    return 0.0;
  }
}

std::optional<double> SVGGeometryElement::pathLength() const {
  if (const auto* component = handle_.try_get<components::PathLengthComponent>()) {
    return component->value;
  } else {
    return std::nullopt;
  }
}

void SVGGeometryElement::setPathLength(std::optional<double> value) {
  if (value) {
    handle_.emplace_or_replace<components::PathLengthComponent>(value.value());
  } else {
    handle_.remove<components::PathLengthComponent>();
  }
}

std::optional<Path> SVGGeometryElement::computedSpline() const {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (const components::ComputedPathComponent* computedPath =
          components::ShapeSystem().createComputedPathIfShape(handle_, FontMetrics(), disabledSink)) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

std::optional<Box2d> SVGGeometryElement::worldBounds() const {
  return components::ShapeSystem().getShapeWorldBounds(handle_);
}

void SVGGeometryElement::invalidate() {
  handle_.remove<components::ComputedPathComponent>();
  handle_.get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::Shape | components::DirtyFlagsComponent::RenderInstance);
}

}  // namespace donner::svg
