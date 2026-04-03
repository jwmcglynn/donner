#include "donner/svg/SVGFEDropShadowElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEDropShadowElement SVGFEDropShadowElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEDropShadowComponent>();
  return SVGFEDropShadowElement(handle);
}

}  // namespace donner::svg
