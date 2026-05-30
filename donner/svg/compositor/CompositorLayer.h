#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "donner/base/Box.h"
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

/// Format `reasons` as a pipe-separated list of set flag names, e.g.
/// `"Filter | IsolatedLayer"`. Returns `"None"` when no flags are set.
/// Used by the editor's layer-inspector panel to render the "Kind" column.
[[nodiscard]] std::string FallbackReasonToString(FallbackReason reasons);

/// Immediate-mode eligibility and timing diagnostics for a promoted layer.
struct ImmediateLayerPlan {
  /// True when editor-facing presentation treats the layer as a transient immediate tile.
  bool immediate = false;
  /// True when the static cost heuristic chose immediate presentation.
  bool staticHeuristicImmediate = false;
  /// True when measured raster time expanded this layer into immediate presentation.
  bool dynamicHeuristicImmediate = false;
  /// True when this layer left dynamic immediate mode because the latest render was over budget.
  bool demotedDynamicImmediate = false;
  /// Snapped canvas-space bounds used by the immediate/cached heuristic.
  Box2d boundsCanvas;
  /// Estimated number of direct geometry draws in the layer.
  int estimatedDrawOps = 0;
  /// Estimated number of path verbs across direct geometry draws.
  int estimatedPathVerbs = 0;
  /// True when the layer uses effects or resources that force cached presentation.
  bool hasExpensiveEffect = false;
  /// True when the layer has a visible, bounded contribution to the canvas.
  bool visible = false;
  /// Estimated presentation texture bytes retained by a cached tile.
  uint64_t estimatedRetainedBytes = 0;
  /// Relative redraw cost from tight area and geometry complexity.
  double estimatedRedrawCost = 0.0;
  /// Relative fixed/cache memory cost avoided by immediate presentation.
  double estimatedCacheOverheadCost = 0.0;
  /// Raster time from the most recent layer render.
  double measuredRasterizeMs = 0.0;
  /// Total dynamic immediate-layer frame budget for 120 Hz interaction.
  double immediateBudgetMs = 0.0;
  /// Budget charged by this layer when it is immediate.
  double immediateBudgetChargeMs = 0.0;
};

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

  /// Returns the cached GPU texture for this layer, if the renderer produced one.
  [[nodiscard]] const std::shared_ptr<const RendererTextureSnapshot>& textureSnapshot() const {
    return textureSnapshot_;
  }

  /// Returns the `canvasFromBitmap` transform applied during blitting.
  /// Maps bitmap-local pixel coordinates (origin at the bitmap's top
  /// left, i.e. the rasterize-time coordinate frame) into canvas pixels.
  /// Identity when the DOM entity has not moved since rasterization; a
  /// pure translation during the bitmap-reuse fast path; other
  /// transforms force re-rasterization before compose.
  ///
  /// When the bitmap is *intrinsic-sized* (smaller than the canvas,
  /// rasterized into a tight bound — see `canvasOffset()`), the full
  /// compose transform is `Translate(canvasOffset) * canvasFromBitmap`.
  /// `canvasFromBitmap_` itself still encodes only the post-rasterize
  /// drift, so the fast-path math is unchanged.
  [[nodiscard]] const Transform2d& canvasFromBitmap() const { return canvasFromBitmap_; }

  /// Canvas-space top-left position of the bitmap's pixel (0,0). Zero
  /// for canvas-sized layers (the rasterize spans the whole canvas and
  /// the bitmap's origin is the canvas origin). Non-zero for intrinsic-
  /// sized layers, where the rasterize covers only the entity's
  /// world-bounds + filter padding and the bitmap is placed back into
  /// the canvas at `canvasOffset`.
  ///
  /// See design doc 0033 §M2 "Intrinsic-size layer rasterization".
  [[nodiscard]] const Vector2d& canvasOffset() const { return canvasOffset_; }

  /// Set the canvas-space top-left where this layer's bitmap blits back.
  /// Called by `CompositorController::rasterizeLayer` immediately after
  /// `setBitmap` when the layer was rasterized into a tight-bound
  /// offscreen.
  void setCanvasOffset(const Vector2d& offset) { canvasOffset_ = offset; }

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

  /// Returns true if the layer has any renderable cached payload.
  [[nodiscard]] bool hasRenderablePayload() const {
    return hasValidBitmap() || textureSnapshot_ != nullptr;
  }

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
    textureSnapshot_.reset();
    bitmapEntityFromWorldTransform_ = worldFromEntityTransform;
    // Reset the compose offset: the new bitmap captures the entity at
    // `worldFromEntityTransform` (its CURRENT world position), so no
    // additional offset is needed to display it at that position.
    //
    // Any `canvasFromBitmap_` value that was computed against the
    // PREVIOUS (stale) bitmap stamp — e.g. the update loop in
    // `renderFrame` that sets `delta = current_RIC - old_stamp`
    // before `rasterizeDirtyLayersLoop` fires — is wrong for the new
    // bitmap: displaying at `new_stamp + old_delta` double-offsets the
    // element by (current - old). Resetting here brings the post-
    // rasterize compose math back to "draw bitmap at stamped pos."
    //
    // Callers that want a specific compose offset on top of the
    // freshly-rasterized bitmap (e.g. a drag hand-off that wants to
    // preserve the in-flight screen position across the rasterize)
    // must call `setCanvasFromBitmap` explicitly AFTER this.
    canvasFromBitmap_ = Transform2d();
    dirty_ = false;
    ++generation_;
    ++rasterizeCount_;
  }

  /// Set the cached GPU texture for this layer.
  void setTextureSnapshot(std::shared_ptr<const RendererTextureSnapshot> texture,
                          const Transform2d& worldFromEntityTransform) {
    bitmap_ = RendererBitmap{};
    textureSnapshot_ = std::move(texture);
    bitmapEntityFromWorldTransform_ = worldFromEntityTransform;
    canvasFromBitmap_ = Transform2d();
    dirty_ = false;
    ++generation_;
    ++rasterizeCount_;
  }

  /// Monotonic version counter — bumped on every `setBitmap`. The
  /// editor uses it to decide whether to re-upload this layer's
  /// bitmap to its cached GL texture.
  [[nodiscard]] uint64_t generation() const { return generation_; }

  /// Override the generation with a controller-supplied value. Per-object
  /// `++generation_` resets to 1 whenever a fresh `CompositorLayer` is built
  /// (e.g. after `resetAllLayers` on document replace). Because entity ids are
  /// reused across a document swap, that fresh "1" can collide with the
  /// generation the editor already cached for the *previous* document's layer
  /// at the same entity id, suppressing the re-upload and leaving stale pixels
  /// on screen. `CompositorController` calls this with a process-monotonic
  /// counter (shared with static segments) so each rasterization is globally
  /// unique. See `CompositorController::rasterizeLayer`.
  void setGeneration(uint64_t generation) { generation_ = generation; }

  /// Cumulative number of times this layer's bitmap has been re-rasterized
  /// since construction. Increments with `generation_`, but is exposed as
  /// a separate counter so the editor's layer-inspector panel can show a
  /// "rasterize count" column without ambiguity over what `generation`
  /// means (the editor uses `generation` for GL texture upload gating).
  [[nodiscard]] uint32_t rasterizeCount() const { return rasterizeCount_; }

  /// Wall-clock milliseconds the most recent `rasterizeLayer` call took,
  /// as measured by the compositor. Zero if the layer has never been
  /// rasterized. See `CompositorController::snapshotLayerInspectorRows`.
  [[nodiscard]] double lastRasterizeMs() const { return lastRasterizeMs_; }

  /// Record the wall-clock duration of the most recent rasterization.
  /// Called by `CompositorController::rasterizeLayer` immediately after
  /// `setBitmap`.
  void setLastRasterizeMs(double ms) { lastRasterizeMs_ = ms; }

  /// Returns immediate-mode eligibility and timing diagnostics for this layer.
  [[nodiscard]] const ImmediateLayerPlan& immediatePlan() const { return immediatePlan_; }

  /// Set immediate-mode eligibility and timing diagnostics for this layer.
  void setImmediatePlan(ImmediateLayerPlan plan) { immediatePlan_ = plan; }

  /// Returns true when editor-facing presentation treats this layer as a transient immediate tile.
  [[nodiscard]] bool isImmediate() const { return immediatePlan_.immediate; }

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
  std::shared_ptr<const RendererTextureSnapshot> textureSnapshot_;
  std::optional<Transform2d> bitmapEntityFromWorldTransform_;
  /// `canvasFromBitmap` transform applied during blitting — see the
  /// public accessor for the full semantics.
  Transform2d canvasFromBitmap_;
  FallbackReason fallbackReasons_ = FallbackReason::None;
  bool dirty_ = true;
  uint64_t generation_ = 0;
  uint32_t rasterizeCount_ = 0;
  double lastRasterizeMs_ = 0.0;
  Vector2d canvasOffset_ = Vector2d::Zero();
  ImmediateLayerPlan immediatePlan_;
};

}  // namespace donner::svg::compositor
