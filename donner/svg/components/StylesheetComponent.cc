#include "donner/svg/components/StylesheetComponent.h"

#include "donner/svg/properties/PresentationAttributeParsing.h"

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Style>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

}  // namespace donner::svg::parser
