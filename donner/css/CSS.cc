#include "donner/css/CSS.h"

#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::css {

Stylesheet CSS::ParseStylesheet(std::string_view str) {
  return parser::StylesheetParser::Parse(str);
}

std::vector<Declaration> CSS::ParseStyleAttribute(std::string_view str) {
  return parser::DeclarationListParser::ParseOnlyDeclarations(str);
}

std::optional<Selector> CSS::ParseSelector(std::string_view str) {
  if (auto maybeSelector = parser::SelectorParser::Parse(str); maybeSelector.hasResult()) {
    return std::move(maybeSelector.result());
  } else {
    return std::nullopt;
  }
}

}  // namespace donner::css
