#include "donner/svg/SVGPolygonElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/PolyComponent.h"

namespace donner::svg {

SVGPolygonElement SVGPolygonElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGPolygonElement(handle);
}

void SVGPolygonElement::setPoints(std::vector<Vector2d> points) {
  invalidate();
  handle_.emplace_or_replace<components::PolyComponent>(components::PolyComponent::Type::Polygon)
      .points = std::move(points);
}

const std::vector<Vector2d>& SVGPolygonElement::points() const {
  return handle_.get_or_emplace<components::PolyComponent>(components::PolyComponent::Type::Polygon)
      .points;
}

void SVGPolygonElement::invalidate() const {
  handle_.remove<components::ComputedPathComponent>();
}

}  // namespace donner::svg
