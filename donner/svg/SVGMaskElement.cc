#include "donner/svg/SVGMaskElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"

namespace donner::svg {

SVGMaskElement SVGMaskElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::MaskComponent>();
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  return SVGMaskElement(handle);
}

MaskUnits SVGMaskElement::maskUnits() const {
  return handle_.get<components::MaskComponent>().maskUnits.value_or(MaskUnits::Default);
}

void SVGMaskElement::setMaskUnits(MaskUnits value) {
  handle_.get<components::MaskComponent>().maskUnits = value;
}

MaskContentUnits SVGMaskElement::maskContentUnits() const {
  return handle_.get<components::MaskComponent>().maskContentUnits.value_or(
      MaskContentUnits::Default);
}

void SVGMaskElement::setMaskContentUnits(MaskContentUnits value) {
  handle_.get<components::MaskComponent>().maskContentUnits = value;
}

}  // namespace donner::svg
