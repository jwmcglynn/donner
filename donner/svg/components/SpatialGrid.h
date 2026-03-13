#pragma once
/// @file

#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/SmallVector.h"

namespace donner::svg::components {

/**
 * Uniform grid spatial index for O(1) point queries.
 *
 * Divides the world-space bounding area into a grid of equal-sized cells. Each cell stores
 * entities whose world-space AABBs overlap that cell. Point queries look up the single cell
 * containing the point and return its entities.
 */
class SpatialGrid {
public:
  /// Construct a grid covering `worldBounds` with cells of approximately `cellSize`.
  SpatialGrid(const Boxd& worldBounds, double cellSize);

  /// Default constructor: creates an empty, zero-size grid.
  SpatialGrid() = default;

  /// Insert an entity with its world-space AABB and draw order.
  void insert(Entity entity, const Boxd& bounds, int drawOrder);

  /// Remove an entity from the grid.
  void remove(Entity entity);

  /// Return all entities whose cells contain `point`, sorted by drawOrder descending
  /// (front-to-back).
  SmallVector<Entity, 8> query(const Vector2d& point) const;

  /// Return all entities whose cells overlap `rect`, sorted by drawOrder descending.
  SmallVector<Entity, 16> queryRect(const Boxd& rect) const;

  /// Rebuild the grid from the current set of RenderingInstanceComponent in the registry.
  /// Uses ShapeSystem::getShapeWorldBounds for AABB computation.
  void rebuild(Registry& registry);

  /// Incrementally update the grid for entities whose bounds have changed.
  /// Removes and re-inserts each entity with its current world-space AABB.
  /// More efficient than a full rebuild when only a few entities are dirty.
  ///
  /// @param registry The ECS registry
  /// @param dirtyEntities Entities (render entities with RenderingInstanceComponent) to update
  void updateDirtyEntities(Registry& registry, std::span<const Entity> dirtyEntities);

  /// Return the number of entities in the grid.
  size_t size() const { return entityCells_.size(); }

  /// Return true if the grid has been built (has non-zero dimensions).
  bool isBuilt() const { return cols_ > 0 && rows_ > 0; }

private:
  struct CellEntry {
    int drawOrder;
    Entity entity;
  };

  /// Convert a world-space x coordinate to a column index, clamped to grid bounds.
  int colForX(double x) const;
  /// Convert a world-space y coordinate to a row index, clamped to grid bounds.
  int rowForY(double y) const;
  /// Get the cell index from column and row.
  int cellIndex(int col, int row) const { return row * cols_ + col; }

  Boxd worldBounds_;
  double cellSize_ = 0.0;
  int cols_ = 0;
  int rows_ = 0;

  /// Each cell stores entities overlapping it, sorted by drawOrder descending.
  std::vector<SmallVector<CellEntry, 4>> cells_;

  /// Reverse map: entity -> cell indices it occupies.
  entt::dense_map<Entity, SmallVector<int, 4>> entityCells_;
};

}  // namespace donner::svg::components
