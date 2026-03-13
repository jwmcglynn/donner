#include "donner/svg/components/StylesheetComponent.h"  // IWYU pragma: keep

#include "donner/css/Stylesheet.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::svg::components {

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str) {
  stylesheet = donner::css::parser::StylesheetParser::Parse(str);
}

}  // namespace donner::svg::components
