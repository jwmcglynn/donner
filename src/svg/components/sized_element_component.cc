#include "src/svg/components/sized_element_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner {

// SVGSVGElement shares this component.
template <>
ParseResult<bool> svg::ParsePresentationAttribute<ElementType::SVG>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

// SVGUseElement shares this component.
template <>
ParseResult<bool> svg::ParsePresentationAttribute<ElementType::Use>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

}  // namespace donner
