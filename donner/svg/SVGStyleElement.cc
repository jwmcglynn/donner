#include "donner/svg/SVGStyleElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/StylesheetComponent.h"

namespace donner::svg {

SVGStyleElement SVGStyleElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  return SVGStyleElement(handle);
}

void SVGStyleElement::setType(const RcStringOrRef& type) {
  auto& stylesheetComponent = handle_.get_or_emplace<components::StylesheetComponent>();
  stylesheetComponent.type = RcString(type);
}

void SVGStyleElement::setContents(std::string_view style) {
  if (isCssType()) {
    auto& stylesheetComponent = handle_.get_or_emplace<components::StylesheetComponent>();
    components::FontContext* fontCtx = nullptr;
    if (handle_.registry()->ctx().contains<components::FontContext>()) {
      fontCtx = &handle_.registry()->ctx().get<components::FontContext>();
    }
    stylesheetComponent.parseStylesheet(style, fontCtx);
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
