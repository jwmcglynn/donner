#include "donner/svg/SVGMetadataElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/TreeMutation.h"
#include "donner/svg/components/style/SelectorContent.h"

namespace donner::svg {

SVGMetadataElement SVGMetadataElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  // `<metadata>` is a descriptive element: it and its children are never rendered.
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  return SVGMetadataElement(handle);
}

RcString SVGMetadataElement::textContent() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::DescriptiveTextComponent>();
  return component != nullptr ? component->text : RcString("");
}

void SVGMetadataElement::setTextContent(std::string_view text) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  const EntityHandle selectorHandle = unsafeEntityHandle();
  const bool wasEmpty =
      components::IsSelectorEmpty(*selectorHandle.registry(), selectorHandle.entity(), nullptr);
  components::ResetSelectorTextContent(unsafeEntityHandle());
  auto& component = handle_.get_or_emplace<components::DescriptiveTextComponent>(access);
  component.text = RcString(text);
  if (wasEmpty !=
      components::IsSelectorEmpty(*selectorHandle.registry(), selectorHandle.entity(), nullptr)) {
    components::TreeMutation::InvalidateTextContentStyles(selectorHandle);
  }
}

}  // namespace donner::svg
