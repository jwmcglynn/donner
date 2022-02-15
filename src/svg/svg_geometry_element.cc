#include "src/svg/svg_geometry_element.h"

#include "src/svg/components/path_length_component.h"

namespace donner::svg {

std::optional<double> SVGGeometryElement::pathLength() const {
  if (const auto* component = handle_.try_get<PathLengthComponent>()) {
    return component->value;
  } else {
    return std::nullopt;
  }
}

void SVGGeometryElement::setPathLength(std::optional<double> value) {
  if (value) {
    handle_.emplace_or_replace<PathLengthComponent>(value.value());
  } else {
    handle_.remove<PathLengthComponent>();
  }
}

}  // namespace donner::svg
