#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGFEFuncAElement.h"
#include "donner/svg/SVGFEFuncBElement.h"
#include "donner/svg/SVGFEFuncGElement.h"
#include "donner/svg/SVGFEFuncRElement.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEFuncAElement SVGFEFuncAElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::A);
  return SVGFEFuncAElement(handle);
}

SVGFEFuncBElement SVGFEFuncBElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::B);
  return SVGFEFuncBElement(handle);
}

SVGFEFuncGElement SVGFEFuncGElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::G);
  return SVGFEFuncGElement(handle);
}

SVGFEFuncRElement SVGFEFuncRElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFuncComponent>(components::FEFuncComponent::Channel::R);
  return SVGFEFuncRElement(handle);
}

}  // namespace donner::svg
