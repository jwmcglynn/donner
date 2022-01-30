#include "src/svg/svg_style_element.h"

#include "src/svg/components/stylesheet_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGStyleElement SVGStyleElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGStyleElement(registry, CreateEntity(registry, RcString(Tag), Type));
}

void SVGStyleElement::setType(RcString type) {
  auto& stylesheetComponent = registry_.get().get_or_emplace<StylesheetComponent>(entity_);
  stylesheetComponent.type = type;
}

void SVGStyleElement::setContents(std::string_view style) {
  if (isCssType()) {
    auto& stylesheetComponent = registry_.get().get_or_emplace<StylesheetComponent>(entity_);
    stylesheetComponent.parseStylesheet(style);
  }
}

bool SVGStyleElement::isCssType() const {
  auto* stylesheetComponent = registry_.get().try_get<StylesheetComponent>(entity_);
  if (stylesheetComponent) {
    return stylesheetComponent->isCssType();
  } else {
    // If there is no StylesheetComponent, assume the type attribute is empty, which is by default
    // CSS.
    return true;
  }
}

}  // namespace donner
