#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <string>
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

  /// Canvas-space top-left where the tile's bitmap blits back, in
  /// canvas pixels at rasterize time (design doc 0033 §M2C). For
  /// segments this is `staticSegmentOffsets_[i]` (non-zero on the
  /// tight-bounded path, design doc 0027); for layers this is
  /// `CompositorLayer::canvasOffset()` (non-zero for intrinsic-sized
  /// rasters, design doc 0033 §M2A). The editor's blit math is
  /// `Translate(canvasOffsetPx) * canvasFromBitmap`.
  Vector2d canvasOffsetPx = Vector2d::Zero();

  /// `canvasFromBitmap` transform the tile should be drawn with:
  /// maps the bitmap's local pixel grid into canvas pixels. Identity
  /// for segments and for non-drag layers; pure translation for the
  /// drag layer when the fast path has updated its offset.
  Transform2d canvasFromBitmap;

  /// True when this tile is the active drag-target layer. The editor
  /// uses this both for diagnostic highlighting and as the cue to
  /// route the live pre-commit DOM delta through `canvasFromBitmap`
  /// on the GPU side.
  bool isDragTarget = false;
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
///
/// Sized for design-tool workloads on Retina-class displays. The splash
/// at typical zoom is 7 mandatory filter / mask / isolated-layer
/// promotions plus the editor's drag target; each canvas-sized layer
/// at DPR=2 is ~36 MB (4054×2327×4). The pre-M2 budget of 256 MiB was
/// at the limit on the splash and refused new editor promotes once
/// hit — visible to the user via `PromoteRefusalReason::MemoryLimit`
/// in the layer-inspector panel, with the user-visible symptom being
/// "drag uses slow path and view turns low-res after a while" (the
/// canvas-shrink path is what knocks loose enough memory for the
/// next promote to succeed). M3 scale-band caching will reduce
/// per-layer memory by letting layers live at sub-canvas resolution;
/// until then, the budget is sized so a typical splash-class
/// document at typical zoom stays under it with headroom.
inline constexpr size_t kMaxCompositorMemoryBytes = 1024ull * 1024ull * 1024ull;

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
  bool promoteEntity(Entity entity, InteractionHint interactionKind = InteractionHint::ActiveDrag);

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

  /// Canvas-space top-left of the entity's cached layer bitmap. `Zero()`
  /// when the entity is not promoted, when its layer hasn't rasterized
  /// yet, or when the layer fell back to canvas-size rasterization
  /// (design doc 0033 §M2). The editor's `CompositedPreview` consumer
  /// blits the promoted texture at this offset (converted to doc
  /// units) plus the per-frame DOM drag delta.
  [[nodiscard]] Vector2d layerCanvasOffsetOf(Entity entity) const;

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

  /// Max side length (in pixels) of the thumbnail bitmap synthesized into
  /// each `LayerInspectorRow::thumbnailPixels`. The downsample preserves
  /// aspect ratio, so the smaller side is `round(otherSide * shortSide /
  /// longSide)`. 64 px keeps the memory cost trivial (16 KiB / layer max)
  /// while staying legible at the editor's right-pane width.
  static constexpr int kLayerThumbnailMaxSide = 64;

  /// One row of diagnostic state per active compositor layer, intended
  /// for the editor's read-only layer-inspector panel (design doc 0033
  /// M1). Cheap to build, cheap to copy: a few ints and one short
  /// string per layer.
  struct LayerInspectorRow {
    /// Layer's stable numeric id (`CompositorLayer::id`).
    uint32_t layerId = 0;
    /// Root entity of the promoted subtree.
    Entity entity = entt::null;
    /// Pixel size of the layer's cached bitmap. `Vector2i::Zero()` when
    /// the layer has not yet been rasterized (`hasValidBitmap` false).
    Vector2i bitmapSize = Vector2i::Zero();
    /// Monotonic generation counter from `CompositorLayer::generation`.
    uint64_t generation = 0;
    /// Cumulative rasterize count from `CompositorLayer::rasterizeCount`.
    uint32_t rasterizeCount = 0;
    /// Wall-clock duration of the most recent rasterize, in ms.
    double lastRasterizeMs = 0.0;
    /// Whether this layer is currently flagged dirty (a rasterize is
    /// pending on the next `renderFrame` call).
    bool dirty = false;
    /// Whether the layer has a valid cached bitmap.
    bool hasValidBitmap = false;
    /// Raw fallback flags (handy for tests; the panel renders
    /// `fallbackReasonsText` for display).
    FallbackReason fallbackReasons = FallbackReason::None;
    /// Pre-formatted fallback flag list (e.g. `"Filter | IsolatedLayer"`).
    std::string fallbackReasonsText;
    /// Canvas-space top-left position where this layer's bitmap blits
    /// back. `Vector2d::Zero()` for canvas-sized layers; non-zero for
    /// intrinsic-sized layers (design doc 0033 §M2).
    Vector2d canvasOffset = Vector2d::Zero();
    /// Pixel dimensions of the downsampled thumbnail. `Vector2i::Zero()`
    /// when the layer has no valid bitmap. Otherwise the longer side is
    /// `kLayerThumbnailMaxSide` and the shorter side preserves aspect.
    Vector2i thumbnailDims = Vector2i::Zero();
    /// RGBA8 thumbnail pixels, tightly packed (`thumbnailDims.x * 4` row
    /// stride), suitable for direct upload via `glTexImage2D`. Empty when
    /// the layer has no valid bitmap.
    std::vector<uint8_t> thumbnailPixels;
  };

  /// Build a per-layer snapshot of compositor state for the layer-
  /// inspector panel. Safe to call from the renderer worker thread when
  /// the compositor isn't mid-render (the editor calls it at the same
  /// Done-transition point as `fastPathCountersForTesting`). Allocates a
  /// `vector` and one short string per layer — fine for diagnostics, not
  /// hot-path-grade.
  [[nodiscard]] std::vector<LayerInspectorRow> snapshotLayerInspectorRows() const;

  /// One row of diagnostic state per static segment (the non-promoted
  /// content between promoted layers, plus the pre-first and post-last
  /// slots). Sized N+1 where N is `layerCount()`.
  struct SegmentInspectorRow {
    /// Slot index (0..N inclusive). Segment 0 is the pre-first-layer
    /// content; segment N is the post-last-layer content.
    size_t slotIndex = 0;
    /// Pixel dimensions of the cached segment bitmap. `Vector2i::Zero()`
    /// when the slot has no bitmap yet.
    Vector2i bitmapSize = Vector2i::Zero();
    /// Canvas-space top-left offset where this segment's bitmap blits
    /// back (non-zero only on the tight-bounded path, design doc 0027).
    Vector2d canvasOffset = Vector2d::Zero();
    /// Monotonic per-slot generation counter.
    uint64_t generation = 0;
    /// Wall-clock duration of the most recent rasterize, in ms. Zero
    /// when this slot has never been rasterized in the current session.
    double lastRasterizeMs = 0.0;
    /// Whether this slot is currently flagged dirty (a rasterize is
    /// pending on the next `renderFrame` call).
    bool dirty = false;
    /// Whether the slot has a non-empty cached bitmap.
    bool hasValidBitmap = false;
  };

  /// Snapshot per-segment diagnostic state. Mirrors the per-layer
  /// snapshot but covers the static-segment cache, which dominates the
  /// per-frame rasterize cost on documents like the splash. Same
  /// invocation rules as `snapshotLayerInspectorRows`.
  [[nodiscard]] std::vector<SegmentInspectorRow> snapshotSegmentInspectorRows() const;

  /// Compose-output bitmaps (the editor-facing split bitmaps that get
  /// drawn into the canvas pane during drag). Diagnostic-only — the
  /// editor reads the bg/promoted/fg trio directly via
  /// `backgroundBitmap()` / `layerBitmapOf(...)` / `foregroundBitmap()`,
  /// but the layer-inspector panel surfaces this snapshot so the
  /// operator sees every bitmap that contributes to the final compose
  /// at a glance, not just the per-promoted-entity caches.
  struct SplitBitmapsSnapshot {
    bool splitPathActive = false;
    bool hasBackground = false;
    Vector2i backgroundDims = Vector2i::Zero();
    bool hasForeground = false;
    Vector2i foregroundDims = Vector2i::Zero();
    /// Entity whose drag layer the cached bg/fg split is keyed on, or
    /// `entt::null` if the split path isn't active.
    Entity dragEntity = entt::null;
  };

  /// Snapshot the editor-facing split bitmaps. Same invocation rules
  /// as the per-layer / per-segment snapshots.
  [[nodiscard]] SplitBitmapsSnapshot snapshotSplitBitmaps() const;

  /// One row of the unified "everything composited together" view that
  /// the layer-inspector panel renders in paint order — design doc 0033
  /// §M1++. Mirrors what `composeLayers` actually draws so the operator
  /// sees the same sequence of blits the renderer performs.
  ///
  /// Carries a downsampled thumbnail for every tile (background,
  /// foreground, segment, layer) so the panel can render previews
  /// inline instead of just dimensions.
  struct CompositeTileSnapshot {
    enum class Kind : uint8_t {
      Background,
      Foreground,
      Segment,
      Layer,
    };

    Kind kind = Kind::Layer;
    /// Stable identifier for the editor's GL texture cache:
    /// `"bg"`, `"fg"`, `"seg:{index}"`, `"layer:{entity}"`. Lets the
    /// panel re-use uploaded textures across frames for unchanged
    /// tiles.
    std::string id;
    /// Human-readable label rendered in the panel: `"background"`,
    /// `"foreground"`, `"segment 0"`, `"layer #12"`.
    std::string label;
    /// Source bitmap dimensions in canvas pixels. `Vector2i::Zero()`
    /// when the source bitmap is empty.
    Vector2i bitmapDims = Vector2i::Zero();
    /// Monotonic version counter for change detection. Layers / segments
    /// expose real generations; bg/fg use a derived generation that
    /// bumps when the split-bitmap cache is rebuilt.
    uint64_t generation = 0;
    /// Wall-clock duration of the most recent rasterize. Always 0 for
    /// `Background` / `Foreground` (those are *composed*, not
    /// rasterized, by `recomposeSplitBitmaps`).
    double lastRasterizeMs = 0.0;
    /// Whether the source bitmap has pixels.
    bool hasValidBitmap = false;
    /// Whether this tile is the active drag-target layer (highlighted
    /// in the panel). Always false for non-Layer kinds.
    bool isDragTarget = false;
    /// Aspect-preserving downsample (max-side `kLayerThumbnailMaxSide`)
    /// of the source bitmap. Empty when the source has no pixels.
    Vector2i thumbnailDims = Vector2i::Zero();
    std::vector<uint8_t> thumbnailPixels;
  };

  /// Build the unified composite-tile snapshot in paint order. The
  /// sequence mirrors `composeLayers`:
  ///   - When the split-bitmap cache is active (single editor-promoted
  ///     entity): `Background`, `Layer` (drag target), `Foreground`.
  ///     The bg/fg already subsume the static segments and non-drag
  ///     layers below / above the drag entity.
  ///   - Otherwise: `Segment 0`, `Layer 0`, `Segment 1`, `Layer 1`,
  ///     …, `Segment N`. (Editor-facing bg/fg are inactive in this
  ///     mode.)
  [[nodiscard]] std::vector<CompositeTileSnapshot> snapshotCompositeTiles() const;

  /// Compositor-wide state useful for diagnosing why the editor's
  /// expected drag fast path didn't engage. Lets the operator confirm
  /// at a glance:
  ///   - Did the editor's `promoteEntity` for the selected element
  ///     actually take? → `activeHintsCount == 1`.
  ///   - Is the split-bitmap optimization active? → `splitPathActive`.
  ///   - Which entity is the compositor treating as the drag target?
  ///     → `splitStaticLayersEntity`. Compare against the editor's
  ///     selection in the side-panel; a mismatch means the worker
  ///     and editor disagree about what's being dragged.
  ///   - Is the canvas size what the editor expects? → `canvasSize`.
  ///     Drops here explain "the view got pixelated after drag" type
  ///     symptoms.
  /// Reason `promoteEntity` returned false on its most recent call.
  /// Sticky — clears only on a subsequent successful `promoteEntity`.
  /// Surfaced through `StateSnapshot::lastPromoteRefusalReason` so the
  /// editor's diagnostic panel can show "the compositor refused this
  /// promote because <reason>" without the operator having to read
  /// source.
  enum class PromoteRefusalReason : uint8_t {
    None = 0,
    /// Entity was destroyed / never existed in the registry.
    InvalidEntity,
    /// Ancestor has `filter` / `mask` / `clip-path`. Promoting would
    /// lose the ancestor's compositing context.
    CompositingBreakingAncestor,
    /// Too many entities already promoted (`kMaxCompositorLayers`).
    LayerLimit,
    /// Total bitmap memory already at `kMaxCompositorMemoryBytes`.
    MemoryLimit,
    /// A descendant already has its own promoted layer (typically a
    /// mandatorily-detected filter / mask / isolated-layer). Allowing
    /// the parent promote would double-rasterize the descendant.
    DescendantPromoted,
  };

  struct StateSnapshot {
    /// Editor-driven explicit promotions (drag target + selection
    /// prewarm). Mandatory-detector hints don't count toward this
    /// number — they live in a separate map.
    uint32_t activeHintsCount = 0;
    /// Total layers currently in `layers_` (mandatory + explicit).
    uint32_t layerCount = 0;
    /// `hasSplitStaticLayers()` — the editor's drag-overlay fast
    /// path is active when this is true.
    bool splitPathActive = false;
    /// Entity the compositor cached the bg/fg split for. `entt::null`
    /// when split-path is inactive.
    Entity splitStaticLayersEntity = entt::null;
    /// Canvas size the compositor most recently rendered against.
    Vector2i canvasSize = Vector2i::Zero();
    /// Reason the most-recent `promoteEntity` call returned false, or
    /// `None` when the most-recent call succeeded (or no call has
    /// happened yet). Sticky on failure.
    PromoteRefusalReason lastPromoteRefusalReason = PromoteRefusalReason::None;
    /// Entity the failed `promoteEntity` was called with. `entt::null`
    /// when `lastPromoteRefusalReason` is `None`.
    Entity lastPromoteRefusalEntity = entt::null;
  };

  [[nodiscard]] StateSnapshot snapshotState() const;

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
  [[nodiscard]] bool remapAfterStructuralReplace(const std::unordered_map<Entity, Entity>& remap);

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
  [[nodiscard]] bool tightBoundedSegmentsEnabled() const { return config_.tightBoundedSegments; }

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

  /// Read-only accessor for the layer bound to @p entity. Test-only —
  /// lets regression tests inspect `canvasFromBitmap` /
  /// `bitmapEntityFromWorldTransform` after a drag frame to verify the
  /// translation-only fast path engaged (bitmap stamp unchanged,
  /// composition transform carries the delta). Production callers
  /// should go through `isPromoted` / `promoteEntity` instead.
  [[nodiscard]] const CompositorLayer* findLayerForTest(Entity entity) const {
    return findLayer(entity);
  }

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
  /// layer's `canvasFromBitmap`, not re-rasterize). Called AFTER
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
  /// descendant RIC's `worldFromEntityTransform` by @p worldFromPreviousWorld
  /// — the per-frame world-from-world delta that maps a point at its previous
  /// frame's world position to its current world position — so subsequent
  /// reads (e.g. a forced re-rasterize later in the session, or the next
  /// frame's fast-path delta computation against a descendant-rooted layer)
  /// see up-to-date world positions.
  static void propagateFastPathTranslationToSubtree(Registry& registry, Entity root,
                                                    const Transform2d& worldFromPreviousWorld);

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
  /// Per-segment wall-clock duration (ms) of the most recent rasterize,
  /// parallel to `staticSegments_`. Diagnostic-only — surfaced via
  /// `snapshotSegmentInspectorRows` to the layer-inspector panel (design
  /// doc 0033 M1+). Zero for slots that have never rasterized in the
  /// current session.
  std::vector<double> staticSegmentLastRasterizeMs_;
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

  /// Most-recent promote refusal — sticky on failure, cleared on next
  /// successful `promoteEntity`. Diagnostic only.
  PromoteRefusalReason lastPromoteRefusalReason_ = PromoteRefusalReason::None;
  Entity lastPromoteRefusalEntity_ = entt::null;
};

}  // namespace donner::svg::compositor
