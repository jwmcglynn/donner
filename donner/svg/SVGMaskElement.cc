#include "donner/svg/SVGMaskElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"

namespace donner::svg {

SVGMaskElement SVGMaskElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::MaskComponent>();
  handle
      .emplace<components::RenderingBehaviorComponent>(
          components::RenderingBehavior::ShadowOnlyChildren)
      .inheritsParentTransform = false;
  return SVGMaskElement(handle);
}

MaskUnits SVGMaskElement::maskUnits() const {
  return handle_.get<components::MaskComponent>().maskUnits;
}

void SVGMaskElement::setMaskUnits(MaskUnits value) {
  handle_.get<components::MaskComponent>().maskUnits = value;
}

MaskContentUnits SVGMaskElement::maskContentUnits() const {
  return handle_.get<components::MaskComponent>().maskContentUnits;
}

void SVGMaskElement::setMaskContentUnits(MaskContentUnits value) {
  handle_.get<components::MaskComponent>().maskContentUnits = value;
}

void SVGMaskElement::setX(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::MaskComponent>().x = value;
}

void SVGMaskElement::setY(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::MaskComponent>().y = value;
}

void SVGMaskElement::setWidth(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::MaskComponent>().width = value;
}

void SVGMaskElement::setHeight(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::MaskComponent>().height = value;
}

std::optional<Lengthd> SVGMaskElement::x() const {
  return handle_.get_or_emplace<components::MaskComponent>().x;
}

std::optional<Lengthd> SVGMaskElement::y() const {
  return handle_.get_or_emplace<components::MaskComponent>().y;
}

std::optional<Lengthd> SVGMaskElement::width() const {
  return handle_.get_or_emplace<components::MaskComponent>().width;
}

std::optional<Lengthd> SVGMaskElement::height() const {
  return handle_.get_or_emplace<components::MaskComponent>().height;
}

}  // namespace donner::svg
