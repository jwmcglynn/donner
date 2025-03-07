#include "donner/svg/SVGClipPathElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"

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
  return handle_.get<components::ClipPathComponent>().clipPathUnits.value_or(
      ClipPathUnits::Default);
}

void SVGClipPathElement::setClipPathUnits(ClipPathUnits value) {
  handle_.get<components::ClipPathComponent>().clipPathUnits = value;
}

}  // namespace donner::svg
