#include "donner/svg/SVGFEBlendElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEBlendElement SVGFEBlendElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEBlendComponent>();
  return SVGFEBlendElement(handle);
}

}  // namespace donner::svg
