#include "donner/svg/SVGUnknownElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGUnknownElement SVGUnknownElement::CreateOn(EntityHandle handle,
                                              const xml::XMLQualifiedNameRef& tagName) {
  CreateEntityOn(handle, tagName, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  return SVGUnknownElement(handle);
}

}  // namespace donner::svg
