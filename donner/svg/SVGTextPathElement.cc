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
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPathComponent>().href = RcString(href);
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<RcString> SVGTextPathElement::href() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  if (const auto* comp = handle_.try_get<components::TextPathComponent>()) {
    if (!comp->href.empty()) {
      return comp->href;
    }
  }
  return std::nullopt;
}

void SVGTextPathElement::setStartOffset(std::optional<Lengthd> offset) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPathComponent>().startOffset = offset;
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGTextPathElement::startOffset() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  if (const auto* comp = handle_.try_get<components::TextPathComponent>()) {
    return comp->startOffset;
  }
  return std::nullopt;
}

}  // namespace donner::svg
