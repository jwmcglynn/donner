#include "src/svg/svg_polyline_element.h"

#include "src/svg/components/poly_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGPolylineElement SVGPolylineElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGPolylineElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGPolylineElement::setPoints(std::vector<Vector2d> points) {
  invalidate();
  handle_.emplace_or_replace<PolyComponent>(PolyComponent::Type::Polyline).points =
      std::move(points);
}

const std::vector<Vector2d>& SVGPolylineElement::points() const {
  return handle_.get_or_emplace<PolyComponent>(PolyComponent::Type::Polyline).points;
}

void SVGPolylineElement::invalidate() const {
  handle_.remove<ComputedPathComponent>();
}

}  // namespace donner::svg