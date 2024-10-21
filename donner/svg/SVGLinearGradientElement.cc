#include "donner/svg/SVGLinearGradientElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"

namespace donner::svg {

SVGLinearGradientElement SVGLinearGradientElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::LinearGradientComponent>();
  return SVGLinearGradientElement(handle);
}

void SVGLinearGradientElement::setX1(std::optional<Lengthd> value) {
  handle_.get<components::LinearGradientComponent>().x1 = value;
}

void SVGLinearGradientElement::setY1(std::optional<Lengthd> value) {
  handle_.get<components::LinearGradientComponent>().y1 = value;
}

void SVGLinearGradientElement::setX2(std::optional<Lengthd> value) {
  handle_.get<components::LinearGradientComponent>().x2 = value;
}

void SVGLinearGradientElement::setY2(std::optional<Lengthd> value) {
  handle_.get<components::LinearGradientComponent>().y2 = value;
}

std::optional<Lengthd> SVGLinearGradientElement::x1() const {
  return handle_.get<components::LinearGradientComponent>().x1;
}

std::optional<Lengthd> SVGLinearGradientElement::y1() const {
  return handle_.get<components::LinearGradientComponent>().y1;
}

std::optional<Lengthd> SVGLinearGradientElement::x2() const {
  return handle_.get<components::LinearGradientComponent>().x2;
}

std::optional<Lengthd> SVGLinearGradientElement::y2() const {
  return handle_.get<components::LinearGradientComponent>().y2;
}

}  // namespace donner::svg
