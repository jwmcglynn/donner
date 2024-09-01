#include "donner/svg/components/StylesheetComponent.h"  // IWYU pragma: keep

#include "donner/css/Stylesheet.h"
#include "donner/css/parser/StylesheetParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, for ParsePresentationAttribute

namespace donner::svg::components {

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str) {
  stylesheet = donner::css::parser::StylesheetParser::Parse(str);
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Style>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // The style element has no presentation attributes.
  return false;
}

}  // namespace donner::svg::parser
