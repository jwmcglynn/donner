#include "src/svg/svg_polygon_element.h"

#include "src/svg/components/poly_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGPolygonElement SVGPolygonElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGPolygonElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGPolygonElement::setPoints(std::vector<Vector2d> points) {
  invalidate();
  handle_.emplace_or_replace<PolyComponent>(PolyComponent::Type::Polygon).points =
      std::move(points);
}

const std::vector<Vector2d>& SVGPolygonElement::points() const {
  return handle_.get_or_emplace<PolyComponent>(PolyComponent::Type::Polygon).points;
}

void SVGPolygonElement::invalidate() const {
  handle_.remove<ComputedPathComponent>();
}

}  // namespace donner::svg
