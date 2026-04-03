#include "donner/svg/SVGFEPointLightElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEPointLightElement SVGFEPointLightElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::LightSourceComponent>(components::LightSourceComponent::Type::Point);
  return SVGFEPointLightElement(handle);
}

}  // namespace donner::svg
