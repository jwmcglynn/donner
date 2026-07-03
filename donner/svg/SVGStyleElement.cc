#include "donner/svg/SVGStyleElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

SVGStyleElement SVGStyleElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  return SVGStyleElement(handle);
}

void SVGStyleElement::setType(const RcStringOrRef& type) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  auto& stylesheetComponent = handle_.get_or_emplace<components::StylesheetComponent>(access);
  stylesheetComponent.type = RcString(type);
  components::RenderingContext(*handle_.registry()).invalidateRenderTree();
}

void SVGStyleElement::setContents(std::string_view style) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  auto& stylesheetComponent = handle_.get_or_emplace<components::StylesheetComponent>(access);
  if (!stylesheetComponent.isCssType()) {
    mutation.cancel();
    return;
  }

  stylesheetComponent.parseStylesheet(style);
  components::RenderingContext(*handle_.registry()).invalidateRenderTree();
}

RcString SVGStyleElement::textContent() const {
  // The raw CSS text is the authoritative DOM representation of the stylesheet, stored on the
  // StylesheetComponent when the text is parsed. The SVG element tree does not retain the source
  // `Data` child nodes that the XML parse tree carried, so this is the only live text source.
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* stylesheetComponent = handle_.try_get<components::StylesheetComponent>();
  return stylesheetComponent != nullptr ? stylesheetComponent->text : RcString("");
}

void SVGStyleElement::setTextContent(std::string_view text) {
  // `setContents` reparses and stores the raw text on the StylesheetComponent, keeping the parsed
  // rules and `textContent()` coherent.
  setContents(text);
}

bool SVGStyleElement::isCssType() const {
  const auto* stylesheetComponent = handle_.try_get<components::StylesheetComponent>();
  if (stylesheetComponent) {
    return stylesheetComponent->isCssType();
  } else {
    // If there is no StylesheetComponent, assume the type attribute is empty, which is by default
    // CSS.
    return true;
  }
}

}  // namespace donner::svg
