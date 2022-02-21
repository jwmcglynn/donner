#include "src/svg/svg_gradient_element.h"

#include "src/svg/components/gradient_component.h"

namespace donner::svg {

SVGGradientElement::SVGGradientElement(EntityHandle handle) : SVGElement(handle) {
  handle_.emplace<GradientComponent>();
}

GradientUnits SVGGradientElement::gradientUnits() const {
  return handle_.get_or_emplace<GradientComponent>().gradientUnits;
}

void SVGGradientElement::setGradientUnits(GradientUnits value) {
  handle_.get_or_emplace<GradientComponent>().gradientUnits = value;
}

Transformd SVGGradientElement::gradientTransform() const {
  return handle_.get_or_emplace<GradientComponent>().gradientTransform;
}

void SVGGradientElement::setGradientTransform(Transformd value) {
  handle_.get_or_emplace<GradientComponent>().gradientTransform = value;
}

GradientSpreadMethod SVGGradientElement::spreadMethod() const {
  return handle_.get_or_emplace<GradientComponent>().spreadMethod;
}

void SVGGradientElement::setSpreadMethod(GradientSpreadMethod value) {
  handle_.get_or_emplace<GradientComponent>().spreadMethod = value;
}

}  // namespace donner::svg
