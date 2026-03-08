#include "donner/svg/SVGFESpecularLightingElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFESpecularLightingElement SVGFESpecularLightingElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FESpecularLightingComponent>();
  return SVGFESpecularLightingElement(handle);
}

}  // namespace donner::svg
