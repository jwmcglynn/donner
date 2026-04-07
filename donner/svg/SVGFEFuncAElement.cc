#include "donner/svg/SVGFEFuncAElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEFuncAElement SVGFEFuncAElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::A);
  return SVGFEFuncAElement(handle);
}

}  // namespace donner::svg
