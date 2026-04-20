#pragma once
/// @file

#include <cstdint>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor {

/**
 * Flags indicating which compositing features on a promoted entity require conservative fallback
 * (full re-rasterization on every frame rather than bitmap caching).
 *
 * Each flag corresponds to an SVG feature that cannot be correctly rendered in isolation when the
 * entity is composited separately from the rest of the scene.
 */
enum class FallbackReason : uint16_t {
  None = 0,

  /// Entity has mix-blend-mode != normal, requiring interaction with backdrop.
  BlendMode = 1 << 0,

  /// Entity has a filter effect, which may reference sourceGraphic/BackgroundImage.
  Filter = 1 << 1,

  /// Entity has a clip-path referencing elements outside the promoted subtree.
  ClipPath = 1 << 2,

  /// Entity has a mask referencing elements outside the promoted subtree.
  Mask = 1 << 3,

  /// Entity has markers, whose rendering depends on the containing document context.
  Markers = 1 << 4,

  /// Entity has a paint server (gradient/pattern) referencing external elements.
  ExternalPaint = 1 << 5,

  /// Entity establishes an isolation group (opacity < 1 composed with siblings).
  IsolatedLayer = 1 << 6,
};

/// Bitwise OR.
inline constexpr FallbackReason operator|(FallbackReason a, FallbackReason b) {
  return static_cast<FallbackReason>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}

/// Bitwise AND.
inline constexpr FallbackReason operator&(FallbackReason a, FallbackReason b) {
  return static_cast<FallbackReason>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}

/// Bitwise OR assignment.
inline constexpr FallbackReason& operator|=(FallbackReason& a, FallbackReason b) {
  a = a | b;
  return a;
}

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

  /// Returns the `canvasFromBitmap` transform applied during blitting.
  /// Maps bitmap-local pixel coordinates (origin at the bitmap's top
  /// left, i.e. the rasterize-time coordinate frame) into canvas pixels.
  /// Identity when the DOM entity has not moved since rasterization; a
  /// pure translation during the bitmap-reuse fast path; other
  /// transforms force re-rasterization before compose.
  [[nodiscard]] const Transform2d& canvasFromBitmap() const { return canvasFromBitmap_; }

  /// Returns the entity's absolute transform at the moment the cached
  /// bitmap was rasterized, if any. The compositor uses this to decide
  /// whether a subsequent DOM transform mutation can reuse the bitmap
  /// by updating `canvasFromBitmap_` (pure-translation delta) vs
  /// forcing re-rasterization (any other delta).
  [[nodiscard]] const std::optional<Transform2d>& bitmapEntityFromWorldTransform() const {
    return bitmapEntityFromWorldTransform_;
  }

  /// Returns true if the layer needs re-rasterization.
  [[nodiscard]] bool isDirty() const { return dirty_; }

  /// Returns true if the layer has a valid cached bitmap.
  [[nodiscard]] bool hasValidBitmap() const { return !bitmap_.empty(); }

  /// Returns the fallback reasons for this layer, or FallbackReason::None.
  [[nodiscard]] FallbackReason fallbackReasons() const { return fallbackReasons_; }

  /// Returns true if this layer requires conservative fallback (re-rasterize every frame).
  [[nodiscard]] bool requiresConservativeFallback() const {
    return fallbackReasons_ != FallbackReason::None;
  }

  /// @}

  /// @name Mutators
  /// @{

  /// Mark the layer as needing re-rasterization.
  void markDirty() { dirty_ = true; }

  /// Clear the dirty flag after re-rasterization.
  void clearDirty() { dirty_ = false; }

  /// Set the cached bitmap for this layer, along with the entity's
  /// absolute transform at the moment of rasterization. Stored so a
  /// subsequent fast-path DOM translation mutation can detect that the
  /// bitmap's pixel content is still valid (only its world-space
  /// position drifted) and reuse it via a compose-offset delta.
  ///
  /// Bumps `generation_` so the editor can tell a fresh rasterization
  /// from a preserved-across-remap cache via `CompositorTile::
  /// generation` and skip redundant GL texture uploads.
  void setBitmap(RendererBitmap bitmap, const Transform2d& worldFromEntityTransform) {
    bitmap_ = std::move(bitmap);
    bitmapEntityFromWorldTransform_ = worldFromEntityTransform;
    dirty_ = false;
    ++generation_;
  }

  /// Monotonic version counter — bumped on every `setBitmap`. The
  /// editor uses it to decide whether to re-upload this layer's
  /// bitmap to its cached GL texture.
  [[nodiscard]] uint64_t generation() const { return generation_; }

  /// Set the `canvasFromBitmap` transform used during blitting.
  void setCanvasFromBitmap(const Transform2d& transform) { canvasFromBitmap_ = transform; }

  /// Set the fallback reasons for this layer.
  void setFallbackReasons(FallbackReason reasons) { fallbackReasons_ = reasons; }

  /// Update the entity range rendered into this layer.
  void setEntityRange(Entity firstEntity, Entity lastEntity) {
    firstEntity_ = firstEntity;
    lastEntity_ = lastEntity;
  }

  /// Remap the layer's entity ids (`entity_`, `firstEntity_`, `lastEntity_`)
  /// from old to new — used by `CompositorController::remapAfterStructural
  /// Replace` when a structurally-identical document swap gives every
  /// element a new entity id but leaves the rasterized pixels valid. The
  /// cached `bitmap_`, `canvasFromBitmap_`, `bitmapEntityFromWorld
  /// Transform_`, and `fallbackReasons_` survive unchanged — they're
  /// keyed on position-in-paint-order, not entity id.
  void remapEntities(Entity newEntity, Entity newFirstEntity, Entity newLastEntity) {
    entity_ = newEntity;
    firstEntity_ = newFirstEntity;
    lastEntity_ = newLastEntity;
  }

  /// Reassign the layer's numeric id. Used by `CompositorController::reconcileLayers()`
  /// when the resolver reassigns an id for a still-promoted entity (e.g. after a
  /// higher-weight neighbor demotes and this entity shifts up). The cached bitmap,
  /// dirty flag, and composition transform are preserved across an id change.
  void setLayerId(uint32_t id) { id_ = id; }

  /// @}

private:
  uint32_t id_;
  Entity entity_;
  Entity firstEntity_;
  Entity lastEntity_;
  RendererBitmap bitmap_;
  std::optional<Transform2d> bitmapEntityFromWorldTransform_;
  /// `canvasFromBitmap` transform applied during blitting — see the
  /// public accessor for the full semantics.
  Transform2d canvasFromBitmap_;
  FallbackReason fallbackReasons_ = FallbackReason::None;
  bool dirty_ = true;
  uint64_t generation_ = 0;
};

}  // namespace donner::svg::compositor
