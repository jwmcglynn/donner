#include "donner/svg/components/shape/BoundsGeometry.h"

#include <algorithm>
#include <cmath>

#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg::components {

namespace {

bool IsFinite(const Box2d& box) {
  return std::isfinite(box.topLeft.x) && std::isfinite(box.topLeft.y) &&
         std::isfinite(box.bottomRight.x) && std::isfinite(box.bottomRight.y);
}

}  // namespace

std::optional<Box2d> BoundGeometryPieces(std::span<const BoundsGeometryPiece> pieces,
                                         const Transform2d& targetFromSpace) {
  std::optional<Box2d> bounds;
  for (const BoundsGeometryPiece& piece : pieces) {
    const Transform2d targetFromPiece = piece.spaceFromPiece * targetFromSpace;
    const auto* path = std::get_if<std::shared_ptr<const Path>>(&piece.geometry);
    const Box2d box = path ? (*path)->transformedBounds(targetFromPiece)
                           : targetFromPiece.transformBox(std::get<Box2d>(piece.geometry));
    if (!IsFinite(box)) {
      return std::nullopt;
    }
    bounds = bounds ? Box2d::Union(*bounds, box) : box;
  }
  return bounds;
}

bool HasIdenticalPathData(const Path& first, const Path& second) {
  const auto sameCommand = [](const Path::Command& a, const Path::Command& b) {
    return a.verb == b.verb && a.pointIndex == b.pointIndex && a.isInternal == b.isInternal;
  };
  const auto samePoint = [](const Vector2d& a, const Vector2d& b) {
    return a.x == b.x && a.y == b.y;
  };
  return std::equal(first.commands().begin(), first.commands().end(), second.commands().begin(),
                    second.commands().end(), sameCommand) &&
         std::equal(first.points().begin(), first.points().end(), second.points().begin(),
                    second.points().end(), samePoint);
}

std::shared_ptr<const Path> BoundsGeometryPaths::share(const Registry& registry, Entity source,
                                                       const Path& path,
                                                       StyleResourceBudget& budget) {
  std::vector<std::shared_ptr<const Path>>& copies = paths_[{&registry, source}];
  for (const std::shared_ptr<const Path>& copy : copies) {
    budget.noteExternalMeasureGeometryComparison();
    if (HasIdenticalPathData(*copy, path)) {
      return copy;
    }
  }
  const std::optional<std::size_t> bytes = path.retainedBytes();
  if (!bytes || !budget.reserveExternalMeasureGeometryBytes(*bytes)) {
    return nullptr;
  }
  return copies.emplace_back(std::make_shared<const Path>(path));
}

}  // namespace donner::svg::components
