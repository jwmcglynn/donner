#include "donner/svg/SVGFESpotLightElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFESpotLightElement SVGFESpotLightElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::LightSourceComponent>(components::LightSourceComponent::Type::Spot);
  return SVGFESpotLightElement(handle);
}

}  // namespace donner::svg
