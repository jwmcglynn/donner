#include "donner/svg/SVGPolylineElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/PolyComponent.h"

namespace donner::svg {

SVGPolylineElement SVGPolylineElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGPolylineElement(handle);
}

void SVGPolylineElement::setPoints(std::vector<Vector2d> points) {
  invalidate();
  handle_.emplace_or_replace<components::PolyComponent>(components::PolyComponent::Type::Polyline)
      .points = std::move(points);
}

const std::vector<Vector2d>& SVGPolylineElement::points() const {
  return handle_
      .get_or_emplace<components::PolyComponent>(components::PolyComponent::Type::Polyline)
      .points;
}

void SVGPolylineElement::invalidate() const {
  handle_.remove<components::ComputedPathComponent>();
}

}  // namespace donner::svg
