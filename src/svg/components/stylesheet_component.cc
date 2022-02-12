#include "src/svg/components/stylesheet_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner {

template <>
ParseResult<bool> svg::ParsePresentationAttribute<ElementType::Style>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

}  // namespace donner
