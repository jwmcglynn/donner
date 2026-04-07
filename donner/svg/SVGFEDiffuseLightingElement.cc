#include "donner/svg/SVGFEDiffuseLightingElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEDiffuseLightingElement SVGFEDiffuseLightingElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEDiffuseLightingComponent>();
  return SVGFEDiffuseLightingElement(handle);
}

}  // namespace donner::svg
