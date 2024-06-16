#include "src/svg/svg_geometry_element.h"

#include "src/svg/components/path_length_component.h"
#include "src/svg/components/shape/computed_path_component.h"
#include "src/svg/components/shape/shape_system.h"

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

}  // namespace donner::svg
