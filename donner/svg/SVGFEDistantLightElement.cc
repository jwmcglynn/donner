#include "donner/svg/SVGFEDistantLightElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEDistantLightElement SVGFEDistantLightElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::LightSourceComponent>(components::LightSourceComponent::Type::Distant);
  return SVGFEDistantLightElement(handle);
}

}  // namespace donner::svg
