#include "src/svg/components/stylesheet_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Style>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

}  // namespace donner::svg::parser
