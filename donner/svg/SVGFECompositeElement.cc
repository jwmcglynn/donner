#include "donner/svg/SVGFECompositeElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFECompositeElement SVGFECompositeElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FECompositeComponent>();
  return SVGFECompositeElement(handle);
}

}  // namespace donner::svg
