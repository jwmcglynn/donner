#include "donner/svg/SVGTextPathElement.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"

namespace donner::svg {

SVGTextPathElement SVGTextPathElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::TextPathComponent>();

  return SVGTextPathElement(handle);
}

void SVGTextPathElement::setHref(const RcStringOrRef& href) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::TextPathComponent>(access).href = RcString(href);
  invalidateTextGeometry();
}

std::optional<RcString> SVGTextPathElement::href() const {
  if (const auto* comp = handle_.try_get<components::TextPathComponent>()) {
    if (!comp->href.empty()) {
      return comp->href;
    }
  }
  return std::nullopt;
}

void SVGTextPathElement::setStartOffset(std::optional<Lengthd> offset) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::TextPathComponent>(access).startOffset = offset;
  invalidateTextGeometry();
}

std::optional<Lengthd> SVGTextPathElement::startOffset() const {
  if (const auto* comp = handle_.try_get<components::TextPathComponent>()) {
    return comp->startOffset;
  }
  return std::nullopt;
}

}  // namespace donner::svg
