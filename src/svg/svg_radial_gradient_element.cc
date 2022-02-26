#include "src/svg/svg_radial_gradient_element.h"

#include "src/svg/components/radial_gradient_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGRadialGradientElement SVGRadialGradientElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, RcString(Tag), Type);
  handle.emplace<RenderingBehaviorComponent>(RenderingBehavior::Nonrenderable);
  handle.emplace<RadialGradientComponent>();
  return SVGRadialGradientElement(handle);
}

void SVGRadialGradientElement::setCx(std::optional<Lengthd> value) {
  handle_.get<RadialGradientComponent>().cx = value;
}

void SVGRadialGradientElement::setCy(std::optional<Lengthd> value) {
  handle_.get<RadialGradientComponent>().cy = value;
}

void SVGRadialGradientElement::setR(std::optional<Lengthd> value) {
  handle_.get<RadialGradientComponent>().r = value;
}

void SVGRadialGradientElement::setFx(std::optional<Lengthd> value) {
  handle_.get<RadialGradientComponent>().fx = value;
}

void SVGRadialGradientElement::setFy(std::optional<Lengthd> value) {
  handle_.get<RadialGradientComponent>().fy = value;
}

void SVGRadialGradientElement::setFr(std::optional<Lengthd> value) {
  handle_.get<RadialGradientComponent>().fr = value;
}

std::optional<Lengthd> SVGRadialGradientElement::cx() const {
  return handle_.get<RadialGradientComponent>().cx;
}

std::optional<Lengthd> SVGRadialGradientElement::cy() const {
  return handle_.get<RadialGradientComponent>().cy;
}

std::optional<Lengthd> SVGRadialGradientElement::r() const {
  return handle_.get<RadialGradientComponent>().r;
}

std::optional<Lengthd> SVGRadialGradientElement::fx() const {
  return handle_.get<RadialGradientComponent>().fx;
}

std::optional<Lengthd> SVGRadialGradientElement::fy() const {
  return handle_.get<RadialGradientComponent>().fy;
}

std::optional<Lengthd> SVGRadialGradientElement::fr() const {
  return handle_.get<RadialGradientComponent>().fr;
}

}  // namespace donner::svg
