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
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.emplace_or_replace<components::PolyComponent>(components::PolyComponent::Type::Polygon)
      .points = std::move(points);
  access.bumpMutationRevision();
}

const std::vector<Vector2d>& SVGPolygonElement::points() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PolyComponent>();
  return SnapshotPoints(component && component->type == components::PolyComponent::Type::Polygon
                            ? &component->points
                            : nullptr);
}

void SVGPolygonElement::invalidate() const {
  handle_.remove<components::ComputedPathComponent>();
}

}  // namespace donner::svg
