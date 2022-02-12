#include "src/svg/svg_path_element.h"

#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGPathElement SVGPathElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGPathElement(CreateEntity(registry, RcString(Tag), Type));
}

std::string_view SVGPathElement::d() const {
  if (const auto* pathComponent = handle_.try_get<PathComponent>()) {
    return pathComponent->d();
  } else {
    return "";
  }
}

std::optional<ParseError> SVGPathElement::setD(std::string_view d) {
  auto& pathComponent = handle_.get_or_emplace<PathComponent>();
  pathComponent.setD(d);

  auto& computed = handle_.get_or_emplace<ComputedPathComponent>();
  return computed.setFromDString(d);
}

std::optional<double> SVGPathElement::pathLength() const {
  return handle_.get_or_emplace<ComputedPathComponent>().userPathLength;
}

void SVGPathElement::setPathLength(std::optional<double> value) {
  handle_.get_or_emplace<ComputedPathComponent>().userPathLength = value;
}

}  // namespace donner::svg
