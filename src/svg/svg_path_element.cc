#include "src/svg/svg_path_element.h"

#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGPathElement SVGPathElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGPathElement(registry, CreateEntity(registry, "path", Type));
}

std::string_view SVGPathElement::d() const {
  if (const auto* pathComponent = registry_.get().try_get<PathComponent>(entity_)) {
    return pathComponent->d();
  } else {
    return "";
  }
}

std::optional<ParseError> SVGPathElement::setD(std::string_view d) {
  auto& pathComponent = registry_.get().get_or_emplace<PathComponent>(entity_);
  pathComponent.setD(d);

  auto& computed = registry_.get().get_or_emplace<ComputedPathComponent>(entity_);
  return computed.setFromDString(d);
}

}  // namespace donner