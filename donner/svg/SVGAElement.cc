#include "donner/svg/SVGAElement.h"

#include "donner/svg/components/HyperlinkComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGAElement SVGAElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);

  // `<a>` is a transparent grouping element: outside of text it groups arbitrary graphics like
  // `<g>`, so it must traverse its children normally (RenderingBehavior::Default). Inside a text
  // root the renderer never reaches it - the text layout descends through it via the
  // TextComponent/TextPositioningComponent inherited from SVGTextPositioningElement - so it acts as
  // a `<tspan>`-style text-content group without needing NoTraverseChildren.

  return SVGAElement(handle);
}

void SVGAElement::setHref(const std::optional<RcString>& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::HyperlinkComponent>(access).href = value ? *value : RcString();
}

std::optional<RcString> SVGAElement::href() const {
  if (const auto* component = handle_.try_get<components::HyperlinkComponent>()) {
    if (!component->href.empty()) {
      return component->href;
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg
