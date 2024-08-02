#include "donner/svg/SVGGeometryElement.h"

#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg {

double SVGGeometryElement::computedPathLength() const {
  if (const components::ComputedPathComponent* path =
          components::ShapeSystem().createComputedPathIfShape(handle_, FontMetrics(), nullptr)) {
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

std::optional<PathSpline> SVGGeometryElement::computedSpline() const {
  if (const components::ComputedPathComponent* computedPath =
          components::ShapeSystem().createComputedPathIfShape(handle_, FontMetrics(), nullptr)) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

std::optional<Boxd> SVGGeometryElement::worldBounds() const {
  return components::ShapeSystem().getShapeWorldBounds(handle_);
}

void SVGGeometryElement::invalidate() {
  handle_.remove<components::ComputedPathComponent>();
}

}  // namespace donner::svg
