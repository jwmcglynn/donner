#include "donner/svg/SVGRadialGradientElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"

namespace donner::svg {

SVGRadialGradientElement SVGRadialGradientElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::RadialGradientComponent>();
  return SVGRadialGradientElement(handle);
}

void SVGRadialGradientElement::setCx(std::optional<Lengthd> value) {
  handle_.get<components::RadialGradientComponent>().cx = value;
}

void SVGRadialGradientElement::setCy(std::optional<Lengthd> value) {
  handle_.get<components::RadialGradientComponent>().cy = value;
}

void SVGRadialGradientElement::setR(std::optional<Lengthd> value) {
  handle_.get<components::RadialGradientComponent>().r = value;
}

void SVGRadialGradientElement::setFx(std::optional<Lengthd> value) {
  handle_.get<components::RadialGradientComponent>().fx = value;
}

void SVGRadialGradientElement::setFy(std::optional<Lengthd> value) {
  handle_.get<components::RadialGradientComponent>().fy = value;
}

void SVGRadialGradientElement::setFr(std::optional<Lengthd> value) {
  handle_.get<components::RadialGradientComponent>().fr = value;
}

std::optional<Lengthd> SVGRadialGradientElement::cx() const {
  return handle_.get<components::RadialGradientComponent>().cx;
}

std::optional<Lengthd> SVGRadialGradientElement::cy() const {
  return handle_.get<components::RadialGradientComponent>().cy;
}

std::optional<Lengthd> SVGRadialGradientElement::r() const {
  return handle_.get<components::RadialGradientComponent>().r;
}

std::optional<Lengthd> SVGRadialGradientElement::fx() const {
  return handle_.get<components::RadialGradientComponent>().fx;
}

std::optional<Lengthd> SVGRadialGradientElement::fy() const {
  return handle_.get<components::RadialGradientComponent>().fy;
}

std::optional<Lengthd> SVGRadialGradientElement::fr() const {
  return handle_.get<components::RadialGradientComponent>().fr;
}

}  // namespace donner::svg
