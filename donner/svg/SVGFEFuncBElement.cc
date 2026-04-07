#include "donner/svg/SVGFEFuncBElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEFuncBElement SVGFEFuncBElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::B);
  return SVGFEFuncBElement(handle);
}

}  // namespace donner::svg
