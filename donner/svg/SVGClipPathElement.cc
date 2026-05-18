#include "donner/svg/SVGClipPathElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

SVGClipPathElement SVGClipPathElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::ClipPathComponent>();
  handle
      .emplace<components::RenderingBehaviorComponent>(components::RenderingBehavior::Nonrenderable)
      .inheritsParentTransform = false;
  return SVGClipPathElement(handle);
}

ClipPathUnits SVGClipPathElement::clipPathUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::ClipPathComponent>();
  return component ? component->clipPathUnits.value_or(ClipPathUnits::Default)
                   : ClipPathUnits::Default;
}

void SVGClipPathElement::setClipPathUnits(ClipPathUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::ClipPathComponent>().clipPathUnits = value;
  components::RenderingContext(*handle_.registry()).invalidateRenderTree();
  access.bumpMutationRevision();
}

}  // namespace donner::svg
