#include "donner/svg/components/shape/BoundsGeometry.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg::components {

namespace {

bool IsFinite(const Box2d& box) {
  return std::isfinite(box.topLeft.x) && std::isfinite(box.topLeft.y) &&
         std::isfinite(box.bottomRight.x) && std::isfinite(box.bottomRight.y);
}

/// The splitmix64 finalizer: every input bit affects every output bit.
std::uint64_t Avalanche(std::uint64_t value) {
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

/// A hash of @p path's commands and points, equal for paths \ref HasIdenticalPathData matches.
std::size_t PathDataHash(const Path& path) {
  // Coordinates of nearby values differ mostly in their high bits, so each word is fully mixed.
  std::uint64_t hash = 0;
  const auto mix = [&hash](std::uint64_t word) { hash = Avalanche(hash ^ word); };
  // Adding zero maps -0 to +0, which compare equal.
  const auto mixCoordinate = [&mix](double value) {
    mix(std::bit_cast<std::uint64_t>(value + 0.0));
  };
  mix(path.commands().size());
  for (const Path::Command& command : path.commands()) {
    mix((static_cast<std::uint64_t>(command.verb) << 33) |
        (static_cast<std::uint64_t>(command.isInternal) << 32) | command.pointIndex);
  }
  for (const Vector2d& point : path.points()) {
    mixCoordinate(point.x);
    mixCoordinate(point.y);
  }
  return static_cast<std::size_t>(hash);
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
  // Past the allowance nothing more is looked up or copied, so no work grows with the copies.
  if (budget.externalMeasureGeometryExhausted()) {
    return nullptr;
  }
  CopiesByHash& copies = paths_[{&registry, source}];
  const std::size_t hash = PathDataHash(path);
  if (const auto found = copies.find(hash); found != copies.end()) {
    budget.noteExternalMeasureGeometryComparison();
    return HasIdenticalPathData(*found->second, path) ? found->second : nullptr;
  }
  const std::optional<std::size_t> bytes = path.retainedBytes();
  if (!bytes || !budget.reserveExternalMeasureGeometryBytes(*bytes)) {
    return nullptr;
  }
  return copies.emplace(hash, std::make_shared<const Path>(path)).first->second;
}

}  // namespace donner::svg::components
