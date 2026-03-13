#pragma once
/// @file

#include <algorithm>
#include <cstdint>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"

namespace donner::svg {

/**
 * A compositing layer represents a contiguous range of entities in the flat render tree
 * that are rasterized together into a single pixmap. Layers are composed in paint order
 * to produce the final image.
 */
struct CompositingLayer {
  /// Unique ID for this layer (sequential index).
  uint32_t id = 0;

  /// Bounding box in document coordinates (defines pixmap position and size).
  Boxd bounds;

  /// First entity in this layer's range (inclusive), by draw order.
  Entity firstEntity = entt::null;

  /// Last entity in this layer's range (inclusive), by draw order.
  Entity lastEntity = entt::null;

  /// True if this layer needs re-rasterization.
  bool dirty = true;

  /// Reason this layer exists.
  enum class Reason : uint8_t {
    Static,     ///< Background/foreground grouping of unchanged elements.
    Animation,  ///< Contains an animated element.
    Selection,  ///< Contains a user-selected element (editor).
    Explicit,   ///< Programmatic promotion via API.
  };

  /// Why this layer was created.
  Reason reason = Reason::Static;

  /// Transform applied at composition time (e.g., drag offset, viewport pan).
  /// This is composed with the layer's position when drawing the cached pixmap,
  /// without requiring re-rasterization. Pure translations, rotations, and scales
  /// can be applied here for immediate visual feedback.
  Transformd compositionTransform;

  /// Opacity applied at composition time when drawing the cached pixmap.
  /// Allows group opacity changes without re-rasterizing the layer.
  double compositionOpacity = 1.0;
};

/**
 * Result of layer decomposition: an ordered list of layers that cover the entire
 * flat render tree.
 */
struct LayerDecompositionResult {
  /// Layers in paint order. Every entity in the render tree belongs to exactly one layer.
  std::vector<CompositingLayer> layers;
};

/**
 * Abstract interface for determining which entities should be promoted to their own
 * compositing layers.
 *
 * Implementations return the set of "interesting" entities (animated, selected, etc.).
 * The decomposition algorithm handles subtree expansion and compositing context escalation.
 */
class LayerPromotionPolicy {
public:
  virtual ~LayerPromotionPolicy() = default;

  /// Return the set of entities that should be promoted to their own layers.
  virtual std::vector<Entity> getPromotedEntities(const Registry& registry) const = 0;
};

/**
 * Promotes a fixed set of explicitly-selected entities to their own layers.
 * Typical use: editor selection — selected elements get their own layers for
 * interactive manipulation (dragging, rotation) without re-rasterizing.
 */
class SelectionLayerPolicy : public LayerPromotionPolicy {
public:
  /// Set the entities to promote.
  void setSelectedEntities(std::vector<Entity> entities) {
    selectedEntities_ = std::move(entities);
  }

  /// Add an entity to the selection.
  void addEntity(Entity entity) { selectedEntities_.push_back(entity); }

  /// Remove an entity from the selection.
  void removeEntity(Entity entity) {
    selectedEntities_.erase(
        std::remove(selectedEntities_.begin(), selectedEntities_.end(), entity),
        selectedEntities_.end());
  }

  /// Clear the selection.
  void clear() { selectedEntities_.clear(); }

  /// Get the current selection.
  [[nodiscard]] const std::vector<Entity>& selectedEntities() const {
    return selectedEntities_;
  }

  [[nodiscard]] std::vector<Entity> getPromotedEntities(
      const Registry& /*registry*/) const override {
    return selectedEntities_;
  }

private:
  std::vector<Entity> selectedEntities_;
};

}  // namespace donner::svg
