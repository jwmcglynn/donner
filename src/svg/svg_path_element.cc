#include "src/svg/svg_path_element.h"

#include "src/svg/components/path_component.h"

namespace donner {

std::string_view SVGPathElement::d() const {
  if (const auto* pathComponent = registry_.get().try_get<PathComponent>(entity_)) {
    return pathComponent->d();
  } else {
    return "";
  }
}

std::optional<ParseError> SVGPathElement::setD(std::string_view d) {
  auto& pathComponent = registry_.get().get_or_emplace<PathComponent>(entity_);
  return pathComponent.setD(d);
}

}  // namespace donner