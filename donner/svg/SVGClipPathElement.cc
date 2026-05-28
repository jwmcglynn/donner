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
  const auto* component = handle_.try_get<components::ClipPathComponent>();
  return component ? component->clipPathUnits.value_or(ClipPathUnits::Default)
                   : ClipPathUnits::Default;
}

void SVGClipPathElement::setClipPathUnits(ClipPathUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::ClipPathComponent>(access).clipPathUnits = value;
  components::RenderingContext(*handle_.registry()).invalidateRenderTree();
}

}  // namespace donner::svg
