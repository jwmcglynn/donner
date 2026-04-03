#include "donner/svg/SVGFEDisplacementMapElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEDisplacementMapElement SVGFEDisplacementMapElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEDisplacementMapComponent>();
  return SVGFEDisplacementMapElement(handle);
}

}  // namespace donner::svg
