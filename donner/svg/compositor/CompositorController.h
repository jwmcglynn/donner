#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/ComplexityBucketer.h"
#include "donner/svg/compositor/CompositorLayer.h"
#include "donner/svg/compositor/LayerResolver.h"
#include "donner/svg/compositor/MandatoryHintDetector.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor {

/// Maximum number of compositor layers that can be simultaneously active.
inline constexpr int kMaxCompositorLayers = 32;

/// A single bitmap cache unit the compositor exposes for GPU upload —
/// either a static segment (non-promoted content between promoted
/// layers) or a promoted layer's own rasterization.
///
/// The caller (editor) holds a texture per tile keyed on `tileId`, and
/// only re-uploads to the GPU when `generation` advances. The
/// compositor composites these in paint-order (`paintOrderIndex`) on
/// the editor side.
struct CompositorTile {
  /// Stable id for this tile's slot in the compositor's cache
  /// topology. Segments are (0..N), layers are (1'<<31) | entityId so
  /// the two namespaces don't collide. Editor uses the tileId as the
  /// key in its GL texture cache — so as long as the compositor reuses
  /// the same tileId for a preserved bitmap, the editor keeps its
  /// texture.
  uint64_t tileId = 0;

  /// Monotonic version counter, bumped every time this tile's pixel
  /// content is re-rasterized. Editor uploads to GL only when the
  /// generation differs from the one it last uploaded for this tileId.
  /// On the first click-to-drag after page load, at most 3 tile
  /// generations advance — the split segment's two halves and the new
  /// drag-target layer — because every other segment / filter layer
  /// keeps its identity across the layer-set change.
  uint64_t generation = 0;

  /// Position in paint order among the full compositor output. Editor
  /// composites tiles by blitting in ascending `paintOrderIndex`.
  uint32_t paintOrderIndex = 0;

  /// Pointer into the compositor's cached bitmap. Empty if the tile's
  /// slot is structurally present but no pixel content yet (e.g., a
  /// segment whose range contains no visible non-promoted entities).
  const RendererBitmap* bitmap = nullptr;

  /// Non-null for a promoted layer tile: the promoted entity id. Null
  /// for a static segment tile.
  Entity layerEntity = entt::null;

  /// Compose transform the tile should be drawn with. Identity for
  /// segments and for non-drag layers; pure translation for the drag
  /// layer when the fast path has updated its offset.
  Transform2d compositionTransform;
};

/**
 * Walk the XML trees of @p oldDoc and @p newDoc in lockstep and build a
 * remap from old-registry entity ids → new-registry entity ids, suitable
 * for `CompositorController::remapAfterStructuralReplace`. Returns an
 * empty map if the two trees are not structurally equivalent — defined
 * as: same preorder walk, same element tag name at every step, and same
 * `id` attribute at every step (or neither has one). Attribute values
 * other than `id` are allowed to differ — the drag-end writeback case
 * changes `transform` values, not tree shape.
 *
 * The returned map covers every element visited in the walk. The caller
 * is responsible for handing it to `remapAfterStructuralReplace` on the
 * compositor that was previously tracking @p oldDoc's entity space.
 */
[[nodiscard]] std::unordered_map<Entity, Entity> BuildStructuralEntityRemap(
    const SVGDocument& oldDoc, const SVGDocument& newDoc);

/// Maximum total memory budget for compositor layer bitmaps, in bytes.
inline constexpr size_t kMaxCompositorMemoryBytes = 256 * 1024 * 1024;

/**
 * Runtime feature gates for `CompositorController`.
 *
 * Each field toggles an independent auto-promotion source. The primary
 * kill-switch — "don't use the compositor at all" — is a linkage / construction
 * decision: a consumer that doesn't want compositing simply doesn't instantiate
 * a `CompositorController`. These gates only affect what *hint sources* run
 * inside a live compositor.
 *
 * Default-constructed config has all features enabled. Mandatory hints
 * (opacity < 1, filter, mask, blend-mode, isolation) are always active — they
 * implement SVG semantics, not an optional optimization, and cannot be
 * disabled through config.
 *
 * See 0025-composited_rendering.md § Reversibility.
 */
struct CompositorConfig {
  /// Editor-published `InteractionHint` hints promote the selected / dragged
  /// entity to its own layer. When false, the editor falls back to the
  /// explicit `promoteEntity` escape hatch.
  bool autoPromoteInteractions = true;

  /// Animation-system-published hints promote animated subtrees so per-tick
  /// cost stays O(animated subtree). When false, animations re-render the
  /// whole document per tick; selection / drag compositing is unaffected.
  bool autoPromoteAnimations = true;

  /// `ComplexityBucketer` pre-splits the document into a small number of
  /// layers at load / structural rebuild to reduce click-to-first-drag-update
  /// latency. When false, the root layer stays monolithic.
  bool complexityBucketing = true;

  /// When true, `renderFrame` additionally runs a full-document reference
  /// render after the composited path completes and asserts pixel identity
  /// via `UTILS_RELEASE_ASSERT`. Doubles per-frame cost — intended for CI
  /// compositor test targets and `--config=compositor-debug` local runs, not
  /// for interactive editor use. See 0025 § Dual-path debug assertion.
  ///
  /// Default is `false`; CI and debug test configurations flip it on for
  /// the covered test targets.
  bool verifyPixelIdentity = false;

  /// When true, `rasterizeDirtyStaticSegments` calls
  /// `RendererDriver::computeEntityRangeBounds` and sizes each segment's
  /// offscreen bitmap to the tight canvas-space rectangle its contents
  /// paint into (with a 1-px AA padding + 75% coverage cutoff). When
  /// false, every segment rasterizes full-canvas — slower and more
  /// memory, but bypasses every code path added in design doc 0027,
  /// which is the bisection fast-path for any visual regression
  /// suspected to originate in tight-bounded rasterization.
  ///
  /// Flipping the field at runtime (via
  /// `CompositorController::setTightBoundedSegmentsEnabled`) marks all
  /// cached segments dirty so the next frame re-rasterizes under the
  /// new policy. See 0027-tight_bounded_segments.md § Reversibility.
  bool tightBoundedSegments = true;
};

/**
 * Controls compositor layer promotion/demotion and orchestrates composited rendering.
 *
 * The compositor splits the document into layers: one root layer (everything not promoted) and
 * zero or more promoted layers (one per promoted entity subtree). The DOM is the sole source of
 * truth for entity position: during a drag, callers mutate the entity's transform directly
 * (`element.setTransform(...)`) and the compositor's fast path diffs the new absolute transform
 * against the cached bitmap's rasterize-time transform. When the delta is a pure translation, the
 * bitmap is reused and only the internal compose offset updates — no re-rasterization.
 *
 * Usage:
 * ```cpp
 * CompositorController compositor(document, renderer);
 *
 * // Promote drag target
 * compositor.promoteEntity(dragTarget);
 *
 * // During drag — mutate the DOM; the compositor tracks the delta internally.
 * dragElement.setTransform(Transform2d::Translate(dx, dy));
 * compositor.renderFrame(viewport);
 *
 * // When drag ends
 * compositor.demoteEntity(dragTarget);
 * ```
 */
class CompositorController {
public:
  /**
   * Construct a compositor controller.
   *
   * @param document The SVG document to composite.
   * @param renderer The renderer backend to use for rasterization and composition.
   * @param config Runtime feature gates. Default-constructed enables everything.
   */
  CompositorController(SVGDocument& document, RendererInterface& renderer,
                       CompositorConfig config = {});

  /// Returns the runtime config this controller was constructed with.
  [[nodiscard]] const CompositorConfig& config() const { return config_; }

  /// Destructor.
  ~CompositorController();

  // Non-copyable, movable.
  CompositorController(const CompositorController&) = delete;
  CompositorController& operator=(const CompositorController&) = delete;
  CompositorController(CompositorController&&) noexcept;
  CompositorController& operator=(CompositorController&&) noexcept;

  /**
   * Promote an entity to its own compositor layer.
   *
   * The entity and its subtree will be rasterized into a separate bitmap. During composition,
   * the layer bitmap is blitted with its composition transform, avoiding re-rasterization of
   * the rest of the scene.
   *
   * Under `CompositorConfig::autoPromoteInteractions` (default on), this publishes an
   * `Interaction` hint tagged with @p interactionKind. When the gate is off it falls back
   * to an `Explicit` hint, ignoring @p interactionKind.
   *
   * @param entity The entity to promote.
   * @param interactionKind Semantic kind for the Interaction hint. Use `Selection` for
   *   selection-driven pre-warm (no drag in progress) and `ActiveDrag` for an active
   *   user drag. Defaults to `ActiveDrag` for callers that only use this API during drag.
   * @return true if promotion succeeded, false if the layer limit or memory budget was reached.
   */
  bool promoteEntity(Entity entity,
                     InteractionHint interactionKind = InteractionHint::ActiveDrag);

  /**
   * Demote a previously promoted entity back to the root layer.
   *
   * The entity's explicit compositor hint is removed, its computed layer assignment is updated,
   * and the layer is destroyed. The root layer is marked dirty to include the demoted entity on
   * the next render.
   *
   * @param entity The entity to demote. No-op if not currently promoted.
   */
  void demoteEntity(Entity entity);

  /**
   * Returns true if the given entity is currently promoted to its own layer.
   *
   * @param entity The entity to check.
   */
  [[nodiscard]] bool isPromoted(Entity entity) const;

  /**
   * Returns the current bitmap-compose offset for a promoted entity's layer, or identity if the
   * entity is not promoted or has no cached bitmap.
   *
   * The compose offset is the delta between the cached bitmap's rasterize-time world transform
   * and the entity's current absolute world transform. Callers who draw the promoted layer's
   * bitmap independently (e.g. the editor's split-layer display path) must apply this offset so
   * the bitmap aligns with the bg/fg render the compositor just produced.
   *
   * @param entity The entity to query.
   */
  [[nodiscard]] Transform2d layerComposeOffset(Entity entity) const;

  /**
   * Prepare and render a composited frame.
   *
   * This method:
   * 1. Prepares the document (computes styles, layout, render tree) if needed.
   * 2. Checks dirty flags on promoted entities and marks their layers dirty.
   * 3. Re-rasterizes dirty layers via `createOffscreenInstance()` + `drawEntityRange()`.
   * 4. Rasterizes the root layer (everything not in a promoted layer).
   * 5. Composes all layers in paint order via `drawImage()`.
   *
   * @param viewport The viewport for the render pass.
   */
  void renderFrame(const RenderViewport& viewport);

  /**
   * Returns the number of currently active layers (excluding the root layer).
   */
  [[nodiscard]] size_t layerCount() const;

  /**
   * Returns the total memory used by all layer bitmaps, in bytes.
   */
  [[nodiscard]] size_t totalBitmapMemory() const;

  /**
   * Returns a reference to the underlying SVG document.
   */
  [[nodiscard]] SVGDocument& document() { return *document_; }

  /**
   * Returns the fallback reasons for a promoted entity, or FallbackReason::None if not promoted.
   *
   * @param entity The entity to query.
   */
  [[nodiscard]] FallbackReason fallbackReasonsOf(Entity entity) const;

  /// Returns true when the compositor has cached a split underlay/overlay pair for drag preview.
  [[nodiscard]] bool hasSplitStaticLayers() const;

  /// Cached underlay bitmap for the single-promoted-layer drag-preview case.
  [[nodiscard]] const RendererBitmap& backgroundBitmap() const { return backgroundBitmap_; }

  /// Cached overlay bitmap for the single-promoted-layer drag-preview case.
  [[nodiscard]] const RendererBitmap& foregroundBitmap() const { return foregroundBitmap_; }

  /// Cached bitmap for the promoted entity, or an empty bitmap if unavailable.
  [[nodiscard]] const RendererBitmap& layerBitmapOf(Entity entity) const;

  /// Diagnostic counters for the translation-only fast path. Tests read
  /// these to assert that a drag is taking the fast path every frame,
  /// not falling through to `prepareDocumentForRendering`.
  struct FastPathCounters {
    /// Incremented every frame that takes the fast path and successfully
    /// handled every dirty entity via a compose-transform update.
    uint64_t fastPathFrames = 0;
    /// Incremented every frame whose dirty-entity set disqualified the
    /// fast path (transform+other flags, subtree with non-translation
    /// delta, missing layer, etc.) and fell through to the slow path.
    uint64_t slowPathFramesWithDirty = 0;
    /// Incremented every frame where the fast-path eligibility check
    /// wasn't reached because no entities were dirty (e.g. page-load,
    /// selection-change-only frames).
    uint64_t noDirtyFrames = 0;
  };
  [[nodiscard]] const FastPathCounters& fastPathCountersForTesting() const {
    return fastPathCounters_;
  }

  /**
   * Clear all layers and cached state.
   *
   * Two callers, two semantics:
   *
   * - `documentReplaced = false` (the default, used by tests and by the
   *    compositor's own internal paths that reset against the still-live
   *    registry): runs the normal `~ScopedCompositorHint` cleanup, which
   *    removes `CompositorHintComponent`s from the live registry and lets
   *    the resolver strip the now-orphan `ComputedLayerAssignment
   *    Component`s.
   *
   * - `documentReplaced = true` (used by `AsyncRenderer` when it detects
   *    `documentGeneration` has bumped, i.e. a `ReplaceDocumentCommand`
   *    swapped the inner `SVGDocument` at the same optional storage
   *    address): the old Registry was destroyed in place and a brand-new
   *    one constructed at the same address. Every `ScopedCompositorHint`'s
   *    cached `Registry*` now aims at a live object that knows nothing
   *    about the old entity IDs, so calling `registry.valid(old_entity)`
   *    from the dtor SIGSEGVs inside entt's sparse-set lookup. In this
   *    mode the hints are `release()`-defused before clearing — the old
   *    `CompositorHintComponent`s went down with the old registry anyway.
   *
   * After this call, `layerCount()` is 0 and all cached bitmaps are
   * released.  The next `renderFrame()` will do a full render.
   */
  void resetAllLayers(bool documentReplaced = false);

  /**
   * Rewire the compositor's entity-keyed state (`activeHints_`,
   * `mandatoryDetector_`, `complexityBucketer_`, `layers_`) from the
   * old document's entity space onto a new one, after a structurally
   * identical `setDocument`. The cached bitmaps (segments, layer
   * bitmaps, `backgroundBitmap_`, `foregroundBitmap_`) survive
   * untouched — they're keyed on position-in-paint-order, not entity
   * id. This is the fast alternative to `resetAllLayers(documentReplaced
   * =true)` for the editor's drag-end writeback round-trip through
   * `ReplaceDocumentCommand`: with a structurally equal reparse, the
   * new document describes the same render tree with the identical
   * visual result, and the compositor can simply swap ids.
   *
   * @param remap Mapping from old entity id → new entity id. Every
   *   entity in `activeHints_` and in each `CompositorLayer`
   *   (`entity`, `firstEntity`, `lastEntity`) must have an entry;
   *   detectors rebuild against the new registry so their hint set
   *   doesn't need remap entries.
   * @return true on success. On false, the compositor is in an
   *   indeterminate state — the caller MUST follow up with
   *   `resetAllLayers(documentReplaced=true)` to recover.
   */
  [[nodiscard]] bool remapAfterStructuralReplace(
      const std::unordered_map<Entity, Entity>& remap);

  /// Flip tight-bounded segment rasterization on or off at runtime.
  /// See `CompositorConfig::tightBoundedSegments` for semantics. Marks
  /// every cached static segment dirty so the next `renderFrame` call
  /// re-rasterizes under the new policy (otherwise the flip would
  /// affect only segments that happened to get re-rasterized for other
  /// reasons).
  ///
  /// Intended as a bisection knob for the editor: if a visual
  /// regression seems to originate in 0027-tight_bounded_segments, flip
  /// the toggle and watch whether it disappears. Not a hot path —
  /// re-rasterizing every segment on the next frame costs one full
  /// render's worth of work.
  void setTightBoundedSegmentsEnabled(bool enabled);

  /// Returns the current tight-bounded-segments setting. Mirrors
  /// `config().tightBoundedSegments` for convenience.
  [[nodiscard]] bool tightBoundedSegmentsEnabled() const {
    return config_.tightBoundedSegments;
  }

  /// When true, `renderFrame()` skips the main-renderer compose step while
  /// the split-static-layers cache (`bg`/`drag`/`fg` triple) is populated.
  /// The editor's drag overlay reads those bitmaps directly via GL, so the
  /// per-frame `drawImage` calls into the main renderer are wasted work
  /// — on a 892×512 Skia backend with a few filter layers the skip saves
  /// ~100 ms per drag frame. The flat snapshot the editor uploads stays
  /// stale during drag but is only drawn after drag ends, by which point
  /// the settle render (no split cache) has refreshed it.
  void setSkipMainComposeDuringSplit(bool skip) { skipMainComposeDuringSplit_ = skip; }

  /// Enumerate every cacheable unit (static segments + promoted layer
  /// bitmaps) interleaved in paint order. Each tile carries a
  /// `generation` counter that advances only when the tile's pixel
  /// content was actually re-rasterized — so the editor can gate its
  /// GL texture uploads to the minimum set that actually changed this
  /// frame. On a click-to-drag, the user should observe at most 3
  /// tiles advance: the two halves of the split segment and the new
  /// drag-target layer. All other filter layers, segments, and
  /// bucket layers keep their generation and their GL texture binding.
  [[nodiscard]] std::vector<CompositorTile> snapshotTilesForUpload() const;

private:
  /// Find the layer for a given entity, or nullptr if not promoted.
  CompositorLayer* findLayer(Entity entity);
  const CompositorLayer* findLayer(Entity entity) const;

  /// Rasterize a single promoted layer into its bitmap cache.
  void rasterizeLayer(CompositorLayer& layer, const RenderViewport& viewport);

  /// Rasterize any static segments whose `staticSegmentDirty_` flag is
  /// set. Each segment lives between two consecutive promoted layers in
  /// paint-order (plus the pre-first and post-last slots) and is rendered
  /// with all promoted layers hidden + out-of-slot entities hidden. A
  /// mutation inside a segment only re-rasterizes that one segment —
  /// every other segment (and every promoted layer bitmap) stays cached.
  void rasterizeDirtyStaticSegments(const RenderViewport& viewport);

  /// Locate the paint-order segment that contains @p entity. Returns
  /// `layers_.size()` (post-last-layer slot) if the entity lies beyond
  /// every promoted layer or has no `RenderingInstanceComponent`.
  [[nodiscard]] size_t findSegmentForEntity(Entity entity) const;

  /// Rebuild `staticSegments_` / `staticSegmentDirty_` /
  /// `staticSegmentBoundaries_` to match the current `layers_` set,
  /// preserving bitmap caches whose boundary identity (pair of
  /// neighboring layer entity ids) survived the layer set change. Any
  /// slot whose boundary identity is new or shifted gets re-rasterized
  /// on the next `rasterizeDirtyStaticSegments` call. Canvas-size
  /// changes force a full invalidation (cached pixels are sized to the
  /// old canvas, can't be reused).
  ///
  /// Returns true if the layer set or canvas changed enough that the
  /// editor-facing bg/fg cache must be dropped; the caller invalidates
  /// `backgroundBitmap_`/`foregroundBitmap_` accordingly.
  bool resyncSegmentsToLayerSet(const Vector2i& currentCanvasSize);

  /// For each dirty entity that has a Transform/WorldTransform flag,
  /// walk its DOM subtree and mark every static segment that contains
  /// an RIC-bearing descendant as dirty. Stops descent at subtrees
  /// rooted in another promoted layer (those are composed via their
  /// layer's `compositionTransform`, not re-rasterize). Called AFTER
  /// `resyncSegmentsToLayerSet` so the dirty flags aren't overwritten
  /// by the resync's default-per-bitmap-validity logic.
  ///
  /// Needed because a parent-group transform mutates every descendant's
  /// canvas position via the layout cascade, but the baseline
  /// `consumeDirtyFlags` logic only dirties the single segment
  /// containing the mutated entity's own paint-order slot. Without
  /// this, dragging a plain `<g>` leaves descendant segments composed
  /// at pre-drag positions. See compositor golden
  /// `DragGroupWithClipPathSiblingAndGradient`.
  void cascadeTransformDirtyToDescendantSegments(const std::vector<Entity>& dirtyEntities);

  /// Mark every segment dirty (structural rebuilds, layer set changes,
  /// canvas size changes). Resizes `staticSegmentDirty_` to match
  /// `layers_.size() + 1` along the way.
  void markAllSegmentsDirty();

  /// Composite the editor's backwards-compatible bg/fg bitmaps from the
  /// cached static segments plus any non-drag promoted layer bitmaps,
  /// using the drag layer's paint-order position as the split point.
  /// Called from `renderFrame` in the single-drag-target split path after
  /// segments / layers have been rasterized.
  void recomposeSplitBitmaps(const CompositorLayer& dragLayer, const RenderViewport& viewport);

  /// Compose all layers onto the main render target.
  void composeLayers(const RenderViewport& viewport);

  /// Check dirty flags on promoted entities and mark affected layers.
  /// Translate a pre-captured set of dirty entities (snapshotted before
  /// `prepareDocumentForRendering` clears `DirtyFlagsComponent`) into
  /// per-layer `markDirty()` calls. An entity inside a promoted layer's
  /// range marks just that layer; an entity outside every promoted range
  /// escalates to `rootDirty_` so the root bitmap / bg / fg rebuild.
  void consumeDirtyFlags(const std::vector<Entity>& dirtyEntities);

  /// Refresh cached render ranges and fallback metadata after render-tree preparation.
  void refreshLayerMetadata();

  /// Diff `layers_` against the resolver's `ComputedLayerAssignmentComponent` output:
  /// create layers for newly-assigned entities, preserve existing layers' bitmap /
  /// dirty / transform when the assignment is unchanged, reassign ids when the
  /// resolver shifted them, and remove layers whose entity no longer has a
  /// non-zero assignment. This is the single bottleneck every hint source flows
  /// through; `promoteEntity` / `demoteEntity` / `renderFrame` / `resetAllLayers`
  /// all defer to it after running the resolver.
  void reconcileLayers(Registry& registry);

  /// Returns true if the entity is currently within the layer's cached render range.
  bool layerContainsEntity(const CompositorLayer& layer, Entity entity) const;

  /// Compute the entity range for a promoted entity.
  static std::pair<Entity, Entity> computeEntityRange(Registry& registry, Entity entity);

  /// Inspect a RenderingInstanceComponent and return which fallback reasons apply.
  static FallbackReason detectFallbackReasons(
      const components::RenderingInstanceComponent& instance);

  /// Fast-path helper: a promoted subtree layer's root just shifted by a pure
  /// world-space translation. Descendants' local transforms are unchanged, so
  /// their world transforms shift by the same delta. Pre-multiply every
  /// descendant RIC's `worldFromEntityTransform` by @p delta so subsequent
  /// reads (e.g. a forced re-rasterize later in the session, or the next
  /// frame's fast-path delta computation against a descendant-rooted layer)
  /// see up-to-date world positions.
  static void propagateFastPathTranslationToSubtree(Registry& registry, Entity root,
                                                    const Transform2d& delta);

  SVGDocument* document_ = nullptr;
  RendererInterface* renderer_ = nullptr;
  CompositorConfig config_;
  LayerResolver resolver_;
  MandatoryHintDetector mandatoryDetector_;
  ComplexityBucketer complexityBucketer_;
  std::unordered_map<Entity, ScopedCompositorHint> activeHints_;
  std::vector<CompositorLayer> layers_;

  /// Cached static segments — N+1 bitmaps, one per paint-order gap between
  /// promoted layers, plus the pre-first and post-last slots. Together
  /// with `layers_` (interleaved) this reproduces the full document in
  /// correct paint order without ever baking promoted content into the
  /// static caches. Empty vector means segments need (re)rasterization.
  std::vector<RendererBitmap> staticSegments_;
  /// Per-segment dirty flags. When a non-promoted entity mutates we mark
  /// just the segment whose paint-order slot contains it; `rasterizeDirty
  /// StaticSegments` only re-rasterizes segments with `true` here, so the
  /// other segments (and every promoted layer bitmap) stay cached. Always
  /// kept the same size as `staticSegments_`.
  std::vector<bool> staticSegmentDirty_;
  /// Monotonic version counter per static segment, parallel to
  /// `staticSegments_`. Bumps every time a segment's bitmap is re-
  /// rasterized. Used by `snapshotTilesForUpload` to emit a stable
  /// `generation` value per tile so the editor can skip redundant
  /// GL texture uploads when a segment survived the layer-set change
  /// untouched.
  std::vector<uint64_t> staticSegmentGeneration_;
  /// Monotonic counter used to seed `staticSegmentGeneration_[i]` when
  /// a segment slot is freshly created. Survives layer-set shuffles
  /// (i.e., when a segment splits in two, both new slots get fresh
  /// generations from this counter).
  uint64_t nextSegmentGeneration_ = 1;
  /// Boundary identity for each segment in `staticSegments_`. Segment
  /// `i`'s identity is `(left, right)` — the entity ids of the promoted
  /// layers immediately to its left and right in paint order.
  /// `entt::null` in a slot means "document start" (left of `segment[0]`)
  /// or "document end" (right of `segment[N]`). When the layer set
  /// changes (user promotes a drag target, demote on drag-end, etc.),
  /// segments whose boundary pair survives in the new layer set keep
  /// their cached bitmaps; the ones whose boundaries changed (typically
  /// the single segment that the new layer splits) get re-rasterized.
  /// This is what keeps click-to-first-pixel under the 100 ms budget
  /// when the drag target's ancestor segment would otherwise have to
  /// rebuild every cached filter / background bitmap on the first click.
  std::vector<std::pair<Entity, Entity>> staticSegmentBoundaries_;
  /// Per-segment top-left offset in canvas pixels. When the segment's
  /// paint-order range admits a tight canvas-space bound via
  /// `RendererDriver::computeEntityRangeBounds` (accounts for filter
  /// regions, isolated-layer composite halos, stroke extents, etc.),
  /// the segment is rasterized into a pixmap sized to those bounds
  /// with `Translate(-topLeft)` as the base transform. The offset
  /// tells `composeLayers` / `recomposeSplitBitmaps` where on the
  /// canvas to blit the bitmap back.
  ///
  /// `Vector2d::Zero()` means the segment was rasterized full-canvas
  /// (bounds-compute returned `nullopt`, the segment is empty, or the
  /// bounds covered too much of the canvas to be worth the tight
  /// path). See design doc 0027 for the fallback criteria.
  std::vector<Vector2d> staticSegmentOffsets_;
  /// Canvas size the cached segments were rasterized at. Invalidates on
  /// zoom / window resize so stale bitmaps at the old resolution don't
  /// get composed into a new-sized canvas.
  Vector2i staticSegmentsCanvas_ = Vector2i::Zero();
  /// Number of promoted layers when segments were last rasterized. Changing
  /// the promoted set changes segment boundaries, so segments invalidate.
  size_t staticSegmentsLayerCount_ = 0;

  /// Editor-facing split bitmaps — composited from `staticSegments_` plus
  /// any non-drag promoted layer bitmaps. Populated only in the single
  /// drag-target split path for the editor's smooth drag-overlay pipeline.
  RendererBitmap backgroundBitmap_;
  RendererBitmap foregroundBitmap_;
  /// Entity whose drag layer the cached bg/fg split is keyed on. When the
  /// drag target changes, the split must be re-composited even if the
  /// document otherwise appears clean.
  Entity splitStaticLayersEntity_ = entt::null;
  /// Canvas size bg/fg were composited at. Mirrors `staticSegmentsCanvas_`
  /// but tracks bg/fg independently so a no-op frame can skip recompositing.
  Vector2i splitStaticLayersViewport_ = Vector2i::Zero();
  bool rootDirty_ = true;
  bool documentPrepared_ = false;
  /// True once `MandatoryHintDetector` and `ComplexityBucketer` have run
  /// against a populated `RenderingInstanceComponent` view. The first
  /// `renderFrame` call can't scan (RICs don't exist yet), so we defer the
  /// first scan until just after the initial `prepareDocumentForRendering`.
  bool hintsScanned_ = false;
  bool offscreenSupportKnown_ = false;
  bool offscreenSupported_ = false;
  /// When true, `composeLayers` skips the main-renderer draw calls while
  /// the split bg/drag/fg cache is populated — the editor reads those
  /// bitmaps directly, so the main-renderer output would go unconsumed.
  /// See `setSkipMainComposeDuringSplit`.
  bool skipMainComposeDuringSplit_ = false;

  /// True after `composeLayers` has completed a full (non-skipped)
  /// main-renderer compose. Used to gate the skip-compose fast path:
  /// on the first renderFrame of a session the main renderer still
  /// has an empty pixmap, and callers that read `takeSnapshot()` (e.g.
  /// the editor's flat-texture upload, or unit tests that assert the
  /// bitmap is non-empty) expect a real render at least once. We flip
  /// this to true after the first full compose and only skip on
  /// subsequent drag frames, preserving the cached non-split frame as
  /// the flat fallback without ever producing a transparent bitmap.
  bool mainRendererHasCachedFrame_ = false;

  FastPathCounters fastPathCounters_;
};

}  // namespace donner::svg::compositor
