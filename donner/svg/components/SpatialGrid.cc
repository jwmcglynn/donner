#include "donner/svg/components/SpatialGrid.h"

#include <algorithm>

#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg::components {

namespace {

/// Maximum grid dimension in either axis, to prevent excessive memory use.
constexpr int kMaxGridDim = 256;

/// Minimum number of entities before building the grid. Below this threshold, brute-force
/// iteration is fast enough.
constexpr size_t kMinEntitiesForGrid = 16;

/// Minimum cell size in world units, to prevent overly fine grids.
constexpr double kMinCellSize = 8.0;

}  // namespace

SpatialGrid::SpatialGrid(const Boxd& worldBounds, double cellSize)
    : worldBounds_(worldBounds), cellSize_(std::max(cellSize, kMinCellSize)) {
  const double width = worldBounds_.width();
  const double height = worldBounds_.height();

  if (width <= 0.0 || height <= 0.0 || cellSize_ <= 0.0) {
    cols_ = 0;
    rows_ = 0;
    return;
  }

  cols_ = std::min(static_cast<int>(std::ceil(width / cellSize_)), kMaxGridDim);
  rows_ = std::min(static_cast<int>(std::ceil(height / cellSize_)), kMaxGridDim);

  // Ensure at least 1x1.
  cols_ = std::max(cols_, 1);
  rows_ = std::max(rows_, 1);

  // Adjust cellSize_ to exactly cover the world bounds given the clamped grid dimensions.
  cellSize_ = std::max(width / cols_, height / rows_);

  cells_.resize(static_cast<size_t>(cols_) * rows_);
}

int SpatialGrid::colForX(double x) const {
  if (cols_ <= 0) {
    return -1;
  }
  const int col = static_cast<int>((x - worldBounds_.topLeft.x) / cellSize_);
  return std::clamp(col, 0, cols_ - 1);
}

int SpatialGrid::rowForY(double y) const {
  if (rows_ <= 0) {
    return -1;
  }
  const int row = static_cast<int>((y - worldBounds_.topLeft.y) / cellSize_);
  return std::clamp(row, 0, rows_ - 1);
}

void SpatialGrid::insert(Entity entity, const Boxd& bounds, int drawOrder) {
  if (!isBuilt()) {
    return;
  }

  // Compute the column/row range this AABB covers, clamped to grid bounds.
  const int minCol = colForX(bounds.topLeft.x);
  const int maxCol = colForX(bounds.bottomRight.x);
  const int minRow = rowForY(bounds.topLeft.y);
  const int maxRow = rowForY(bounds.bottomRight.y);

  SmallVector<int, 4> cellIndices;

  for (int r = minRow; r <= maxRow; ++r) {
    for (int c = minCol; c <= maxCol; ++c) {
      const int idx = cellIndex(c, r);
      auto& cell = cells_[idx];

      // Insert sorted by drawOrder descending (front-to-back).
      CellEntry entry{drawOrder, entity};
      auto it =
          std::lower_bound(cell.begin(), cell.end(), entry, [](const CellEntry& a, const CellEntry& b) {
            return a.drawOrder > b.drawOrder;
          });
      cell.insert(it, entry);

      cellIndices.push_back(idx);
    }
  }

  entityCells_[entity] = std::move(cellIndices);
}

void SpatialGrid::remove(Entity entity) {
  auto it = entityCells_.find(entity);
  if (it == entityCells_.end()) {
    return;
  }

  const auto& cellIndices = it->second;
  for (const int idx : cellIndices) {
    auto& cell = cells_[idx];
    // Remove all entries matching this entity from the cell.
    auto newEnd = std::remove_if(cell.begin(), cell.end(),
                                 [entity](const CellEntry& e) { return e.entity == entity; });
    // Manually pop entries from the end since SmallVector may not have erase().
    while (cell.end() != newEnd) {
      cell.pop_back();
    }
  }

  entityCells_.erase(it);
}

SmallVector<Entity, 8> SpatialGrid::query(const Vector2d& point) const {
  SmallVector<Entity, 8> result;

  if (!isBuilt()) {
    return result;
  }

  // Check if the point is within the world bounds.
  if (!worldBounds_.contains(point)) {
    return result;
  }

  const int col = colForX(point.x);
  const int row = rowForY(point.y);

  if (col < 0 || row < 0) {
    return result;
  }

  const int idx = cellIndex(col, row);
  const auto& cell = cells_[idx];

  // The cell is already sorted by drawOrder descending. Deduplicate in case of duplicate entries.
  entt::dense_set<Entity> seen;
  for (const auto& entry : cell) {
    if (seen.find(entry.entity) == seen.end()) {
      seen.insert(entry.entity);
      result.push_back(entry.entity);
    }
  }

  return result;
}

SmallVector<Entity, 16> SpatialGrid::queryRect(const Boxd& rect) const {
  SmallVector<Entity, 16> result;

  if (!isBuilt()) {
    return result;
  }

  const int minCol = colForX(rect.topLeft.x);
  const int maxCol = colForX(rect.bottomRight.x);
  const int minRow = rowForY(rect.topLeft.y);
  const int maxRow = rowForY(rect.bottomRight.y);

  if (minCol < 0 || minRow < 0) {
    return result;
  }

  // Collect unique entities from all overlapping cells.
  struct EntityOrder {
    Entity entity;
    int drawOrder;
  };

  entt::dense_set<Entity> seen;
  SmallVector<EntityOrder, 16> collected;

  for (int r = minRow; r <= maxRow; ++r) {
    for (int c = minCol; c <= maxCol; ++c) {
      const int idx = cellIndex(c, r);
      const auto& cell = cells_[idx];
      for (const auto& entry : cell) {
        if (seen.find(entry.entity) == seen.end()) {
          seen.insert(entry.entity);
          collected.push_back({entry.entity, entry.drawOrder});
        }
      }
    }
  }

  // Sort by drawOrder descending (front-to-back).
  std::sort(collected.begin(), collected.end(),
            [](const EntityOrder& a, const EntityOrder& b) { return a.drawOrder > b.drawOrder; });

  for (const auto& eo : collected) {
    result.push_back(eo.entity);
  }

  return result;
}

void SpatialGrid::rebuild(Registry& registry) {
  // Clear existing data.
  cells_.clear();
  entityCells_.clear();
  cols_ = 0;
  rows_ = 0;
  cellSize_ = 0.0;

  // Collect all entities with their bounds and draw orders.
  struct EntityInfo {
    Entity entity;
    Boxd bounds;
    int drawOrder;
  };

  std::vector<EntityInfo> entities;
  ShapeSystem shapeSystem;

  auto view = registry.view<RenderingInstanceComponent>();
  for (auto instanceEntity : view) {
    const auto& instance = view.get<RenderingInstanceComponent>(instanceEntity);
    const Entity dataEntity = instance.dataEntity;

    if (dataEntity == entt::null) {
      continue;
    }

    auto bounds = shapeSystem.getShapeWorldBounds(EntityHandle(registry, dataEntity));
    if (!bounds) {
      continue;
    }

    entities.push_back({instanceEntity, *bounds, instance.drawOrder});
  }

  // Skip grid for small entity counts.
  if (entities.size() < kMinEntitiesForGrid) {
    return;
  }

  // Compute the union of all entity bounds.
  Boxd unionBounds = entities[0].bounds;
  for (size_t i = 1; i < entities.size(); ++i) {
    unionBounds.addBox(entities[i].bounds);
  }

  // Compute cell size heuristic: max(width, height) / sqrt(n), clamped to min 8px.
  const double maxExtent = std::max(unionBounds.width(), unionBounds.height());
  const double heuristicCellSize = maxExtent / std::sqrt(static_cast<double>(entities.size()));
  const double finalCellSize = std::max(heuristicCellSize, kMinCellSize);

  // Re-initialize the grid with computed parameters.
  *this = SpatialGrid(unionBounds, finalCellSize);

  // Insert all entities.
  for (const auto& info : entities) {
    insert(info.entity, info.bounds, info.drawOrder);
  }
}

void SpatialGrid::updateDirtyEntities(Registry& registry, std::span<const Entity> dirtyEntities) {
  if (!isBuilt()) {
    return;
  }

  ShapeSystem shapeSystem;

  for (Entity entity : dirtyEntities) {
    const auto* instance = registry.try_get<RenderingInstanceComponent>(entity);
    if (!instance || instance->dataEntity == entt::null) {
      // Entity no longer has a render instance — remove it from the grid.
      remove(entity);
      continue;
    }

    // Remove old entry.
    remove(entity);

    // Re-insert with updated bounds.
    auto bounds = shapeSystem.getShapeWorldBounds(EntityHandle(registry, instance->dataEntity));
    if (bounds) {
      insert(entity, *bounds, instance->drawOrder);
    }
  }
}

}  // namespace donner::svg::components
