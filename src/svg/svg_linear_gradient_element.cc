#include "src/svg/svg_linear_gradient_element.h"

#include "src/svg/components/linear_gradient_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGLinearGradientElement SVGLinearGradientElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, RcString(Tag), Type);
  handle.emplace<RenderingBehaviorComponent>(RenderingBehavior::Nonrenderable);
  handle.emplace<LinearGradientComponent>();
  return SVGLinearGradientElement(handle);
}

void SVGLinearGradientElement::setX1(Lengthd value) {
  handle_.get<LinearGradientComponent>().x1 = value;
}

void SVGLinearGradientElement::setY1(Lengthd value) {
  handle_.get<LinearGradientComponent>().y1 = value;
}

void SVGLinearGradientElement::setX2(Lengthd value) {
  handle_.get<LinearGradientComponent>().x2 = value;
}

void SVGLinearGradientElement::setY2(Lengthd value) {
  handle_.get<LinearGradientComponent>().y2 = value;
}

Lengthd SVGLinearGradientElement::x1() const {
  return handle_.get<LinearGradientComponent>().x1;
}

Lengthd SVGLinearGradientElement::y1() const {
  return handle_.get<LinearGradientComponent>().y1;
}

Lengthd SVGLinearGradientElement::x2() const {
  return handle_.get<LinearGradientComponent>().x2;
}

Lengthd SVGLinearGradientElement::y2() const {
  return handle_.get<LinearGradientComponent>().y2;
}

}  // namespace donner::svg
