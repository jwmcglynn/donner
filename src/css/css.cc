#include "src/css/css.h"

#include "src/css/parser/declaration_list_parser.h"
#include "src/css/parser/stylesheet_parser.h"

namespace donner::css {

Stylesheet CSS::ParseStylesheet(std::string_view str) {
  return parser::StylesheetParser::Parse(str);
}

std::vector<Declaration> CSS::ParseStyleAttribute(std::string_view str) {
  return parser::DeclarationListParser::ParseOnlyDeclarations(str);
}

}  // namespace donner::css
