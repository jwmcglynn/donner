#pragma once
/// @file

#include <cstdint>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor {

/**
 * Represents a single compositor layer with its cached bitmap and dirty state.
 *
 * Each layer corresponds to a promoted entity subtree. The layer caches its rasterized output and
 * tracks whether re-rasterization is needed (dirty) or only the composition transform has changed.
 */
class CompositorLayer {
public:
  /**
   * Construct a layer for the given entity.
   *
   * @param id Unique layer identifier.
   * @param entity The root entity of this layer.
   * @param firstEntity First entity in the layer's render range (inclusive).
   * @param lastEntity Last entity in the layer's render range (inclusive).
   */
  CompositorLayer(uint32_t id, Entity entity, Entity firstEntity, Entity lastEntity);

  /// @name Accessors
  /// @{

  /// Returns the unique layer identifier.
  [[nodiscard]] uint32_t id() const { return id_; }

  /// Returns the root entity of this layer.
  [[nodiscard]] Entity entity() const { return entity_; }

  /// Returns the first entity in the layer's render range.
  [[nodiscard]] Entity firstEntity() const { return firstEntity_; }

  /// Returns the last entity in the layer's render range.
  [[nodiscard]] Entity lastEntity() const { return lastEntity_; }

  /// Returns the cached bitmap for this layer. Empty if not yet rasterized.
  [[nodiscard]] const RendererBitmap& bitmap() const { return bitmap_; }

  /// Returns the composition transform applied during blitting.
  [[nodiscard]] const Transform2d& compositionTransform() const { return compositionTransform_; }

  /// Returns true if the layer needs re-rasterization.
  [[nodiscard]] bool isDirty() const { return dirty_; }

  /// Returns true if the layer has a valid cached bitmap.
  [[nodiscard]] bool hasValidBitmap() const { return !bitmap_.empty(); }

  /// @}

  /// @name Mutators
  /// @{

  /// Mark the layer as needing re-rasterization.
  void markDirty() { dirty_ = true; }

  /// Clear the dirty flag after re-rasterization.
  void clearDirty() { dirty_ = false; }

  /// Set the cached bitmap for this layer.
  void setBitmap(RendererBitmap bitmap) {
    bitmap_ = std::move(bitmap);
    dirty_ = false;
  }

  /// Set the composition transform used during blitting.
  void setCompositionTransform(const Transform2d& transform) {
    compositionTransform_ = transform;
  }

  /// @}

private:
  uint32_t id_;
  Entity entity_;
  Entity firstEntity_;
  Entity lastEntity_;
  RendererBitmap bitmap_;
  Transform2d compositionTransform_;
  bool dirty_ = true;
};

}  // namespace donner::svg::compositor
