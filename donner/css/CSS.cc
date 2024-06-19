#include "donner/css/CSS.h"

#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::css {

Stylesheet CSS::ParseStylesheet(std::string_view str) {
  return parser::StylesheetParser::Parse(str);
}

std::vector<Declaration> CSS::ParseStyleAttribute(std::string_view str) {
  return parser::DeclarationListParser::ParseOnlyDeclarations(str);
}

}  // namespace donner::css
