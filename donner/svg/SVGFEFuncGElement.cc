#include "donner/svg/SVGFEFuncGElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEFuncGElement SVGFEFuncGElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::G);
  return SVGFEFuncGElement(handle);
}

}  // namespace donner::svg
