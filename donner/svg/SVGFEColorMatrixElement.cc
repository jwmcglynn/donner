#include "donner/svg/SVGFEColorMatrixElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEColorMatrixElement SVGFEColorMatrixElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEColorMatrixComponent>();
  return SVGFEColorMatrixElement(handle);
}

}  // namespace donner::svg
