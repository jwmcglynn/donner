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
};

/**
 * Controls compositor layer promotion/demotion and orchestrates composited rendering.
 *
 * The compositor splits the document into layers: one root layer (everything not promoted) and
 * zero or more promoted layers (one per promoted entity subtree). During interactive drag, only
 * the dragged layer's composition transform changes — no re-rasterization is needed for
 * translation-only drags when the layer's content hasn't changed.
 *
 * Usage:
 * ```cpp
 * CompositorController compositor(document, renderer);
 *
 * // Promote drag target
 * compositor.promoteEntity(dragTarget);
 *
 * // During drag — update composition transform, render composited frame
 * compositor.setLayerCompositionTransform(dragTarget, Transform2d::Translate(dx, dy));
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
   * Set the composition transform for a promoted entity's layer.
   *
   * For translation-only transforms, the layer bitmap is blitted at the new offset without
   * re-rasterization. Non-translation transforms mark the layer dirty for re-rasterization.
   *
   * @param entity The promoted entity.
   * @param transform The composition transform to apply during blitting.
   */
  void setLayerCompositionTransform(Entity entity, const Transform2d& transform);

  /**
   * Returns the current composition transform for a promoted entity, or identity if not promoted.
   *
   * @param entity The entity to query.
   */
  [[nodiscard]] Transform2d compositionTransformOf(Entity entity) const;

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

  /**
   * Clear all layers and cached state.
   *
   * Called when the underlying document is rebuilt (e.g., after `ReplaceDocumentCommand`), which
   * invalidates all entity handles. After this call, `layerCount()` is 0 and all cached bitmaps
   * are released.  The next `renderFrame()` will do a full render.
   */
  void resetAllLayers();

private:
  /// Find the layer for a given entity, or nullptr if not promoted.
  CompositorLayer* findLayer(Entity entity);
  const CompositorLayer* findLayer(Entity entity) const;

  /// Rasterize a single promoted layer into its bitmap cache.
  void rasterizeLayer(CompositorLayer& layer, const RenderViewport& viewport);

  /// Rasterize the root layer (full document render for v1).
  void rasterizeRootLayer(const RenderViewport& viewport);

  /// Rasterize cached underlay/overlay bitmaps for the single promoted-layer drag-preview case.
  void rasterizeSplitRootLayers(const CompositorLayer& layer, const RenderViewport& viewport);

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

  SVGDocument* document_ = nullptr;
  RendererInterface* renderer_ = nullptr;
  CompositorConfig config_;
  LayerResolver resolver_;
  MandatoryHintDetector mandatoryDetector_;
  ComplexityBucketer complexityBucketer_;
  std::unordered_map<Entity, ScopedCompositorHint> activeHints_;
  std::vector<CompositorLayer> layers_;

  /// Cached root layer bitmap (everything not in a promoted layer).
  RendererBitmap rootBitmap_;
  RendererBitmap backgroundBitmap_;
  RendererBitmap foregroundBitmap_;
  /// Entity whose drag layer the cached bg/fg split is keyed on. When the
  /// drag target changes, the split must be re-rasterized even if the
  /// document otherwise appears clean.
  Entity splitStaticLayersEntity_ = entt::null;
  /// Viewport dimensions the cached bg/fg bitmaps were rasterized at. The
  /// cache must invalidate when the viewport resizes (e.g. zoom or window
  /// resize), otherwise stale bitmaps get composed at their old size into
  /// a larger canvas — the content fills only the top-left region and the
  /// rest is transparent.
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
};

}  // namespace donner::svg::compositor
