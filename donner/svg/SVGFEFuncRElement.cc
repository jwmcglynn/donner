#include "donner/svg/SVGFEFuncRElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEFuncRElement SVGFEFuncRElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::R);
  return SVGFEFuncRElement(handle);
}

}  // namespace donner::svg
