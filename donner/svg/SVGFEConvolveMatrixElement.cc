#include "donner/svg/SVGFEConvolveMatrixElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEConvolveMatrixElement SVGFEConvolveMatrixElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEConvolveMatrixComponent>();
  return SVGFEConvolveMatrixElement(handle);
}

}  // namespace donner::svg
