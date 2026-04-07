#include "donner/svg/SVGFEMergeNodeElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEMergeNodeElement SVGFEMergeNodeElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEMergeNodeComponent>();
  return SVGFEMergeNodeElement(handle);
}

}  // namespace donner::svg
