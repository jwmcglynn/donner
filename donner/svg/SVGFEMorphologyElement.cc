#include "donner/svg/SVGFEMorphologyElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEMorphologyElement SVGFEMorphologyElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEMorphologyComponent>();
  return SVGFEMorphologyElement(handle);
}

}  // namespace donner::svg
