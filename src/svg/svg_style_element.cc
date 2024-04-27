#include "src/svg/svg_style_element.h"

#include "src/svg/components/stylesheet_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGStyleElement SVGStyleElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGStyleElement(CreateEntity(registry, Tag, Type));
}

void SVGStyleElement::setType(RcString type) {
  auto& stylesheetComponent = handle_.get_or_emplace<components::StylesheetComponent>();
  stylesheetComponent.type = type;
}

void SVGStyleElement::setContents(std::string_view style) {
  if (isCssType()) {
    auto& stylesheetComponent = handle_.get_or_emplace<components::StylesheetComponent>();
    stylesheetComponent.parseStylesheet(style);
  }
}

bool SVGStyleElement::isCssType() const {
  auto* stylesheetComponent = handle_.try_get<components::StylesheetComponent>();
  if (stylesheetComponent) {
    return stylesheetComponent->isCssType();
  } else {
    // If there is no StylesheetComponent, assume the type attribute is empty, which is by default
    // CSS.
    return true;
  }
}

}  // namespace donner::svg
