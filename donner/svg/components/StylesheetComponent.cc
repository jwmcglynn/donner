#include "donner/svg/components/StylesheetComponent.h"  // IWYU pragma: keep

#include "donner/base/ParseWarningSink.h"
#include "donner/css/Stylesheet.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::svg::components {

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  stylesheet = donner::css::parser::StylesheetParser::Parse(str, disabled);
}

}  // namespace donner::svg::components
