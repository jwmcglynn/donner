#include "donner/svg/SVGFEMergeElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEMergeElement SVGFEMergeElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEMergeComponent>();
  return SVGFEMergeElement(handle);
}

}  // namespace donner::svg
