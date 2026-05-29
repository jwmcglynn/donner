#include "donner/svg/SVGPolygonElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/PolyComponent.h"

namespace donner::svg {
namespace {

const std::vector<Vector2d>& SnapshotPoints(const std::vector<Vector2d>* points) {
  static thread_local std::vector<Vector2d> snapshot;
  if (points) {
    snapshot = *points;
  } else {
    snapshot.clear();
  }
  return snapshot;
}

}  // namespace

SVGPolygonElement SVGPolygonElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGPolygonElement(handle);
}

void SVGPolygonElement::setPoints(std::vector<Vector2d> points) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_
      .emplace_or_replace<components::PolyComponent>(access,
                                                     components::PolyComponent::Type::Polygon)
      .points = std::move(points);
}

const std::vector<Vector2d>& SVGPolygonElement::points() const {
  const auto* component = handle_.try_get<components::PolyComponent>();
  return SnapshotPoints(component && component->type == components::PolyComponent::Type::Polygon
                            ? &component->points
                            : nullptr);
}

void SVGPolygonElement::invalidate() const {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.remove<components::ComputedPathComponent>(access);
}

}  // namespace donner::svg
