#include "donner/svg/compositor/CompositorController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg::compositor {

namespace {

/// Returns true if any ancestor of @p entity carries a clip-path, mask, or
/// filter. When the editor drags a descendant of `<g filter=...>`, promoting
/// the descendant into its own cached layer loses the ancestor's filter
/// context. The compositor's cache path has no way to replay that ancestor
/// filter during composition, so we refuse the promotion and fall back to the
/// non-composited path.
///
/// Checks three signals so the check works both before and after
/// `prepareDocumentForRendering`:
///   1. Raw XML attributes (`filter`, `mask`, `clip-path`) on
///      `AttributesComponent`. Present immediately after parse.
///   2. Resolved fields on `RenderingInstanceComponent` (post-prepare).
///   3. `isolatedLayer` on `RenderingInstanceComponent` (opacity<1,
///      blend-mode, isolation:isolate — any of which make the ancestor a
///      compositing group that can't be extracted from).
bool HasCompositingBreakingAncestor(Registry& registry, Entity entity) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
  if (tree == nullptr) {
    return false;
  }
  Entity cursor = tree->parent();
  while (cursor != entt::null && registry.valid(cursor)) {
    // Raw-attribute check. The parser attaches `AttributesComponent` to every
    // XML element; `<g filter="..." mask="..." clip-path="...">` shows up here
    // before any resolver has run.
    if (const auto* attrs = registry.try_get<donner::components::AttributesComponent>(cursor)) {
      if (attrs->hasAttribute(xml::XMLQualifiedNameRef("filter")) ||
          attrs->hasAttribute(xml::XMLQualifiedNameRef("mask")) ||
          attrs->hasAttribute(xml::XMLQualifiedNameRef("clip-path"))) {
        return true;
      }
    }
    // Resolved-field check. Covers post-prepare state and also covers cases
    // where the filter/mask/clip came from CSS rather than an attribute.
    if (const auto* ancestorInstance =
            registry.try_get<components::RenderingInstanceComponent>(cursor)) {
      if (ancestorInstance->clipPath.has_value() || ancestorInstance->mask.has_value() ||
          ancestorInstance->resolvedFilter.has_value() || ancestorInstance->isolatedLayer) {
        return true;
      }
    }
    const auto* ancestorTree = registry.try_get<donner::components::TreeComponent>(cursor);
    if (ancestorTree == nullptr) {
      break;
    }
    cursor = ancestorTree->parent();
  }
  return false;
}

/// True if @p maybeDescendant is a non-strict descendant of @p root in the DOM
/// tree. Used by cache-range overlap guards before reusing promoted subtree
/// bitmaps.
bool IsDomDescendantOf(Registry& registry, Entity maybeDescendant, Entity root) {
  if (maybeDescendant == root || !registry.valid(maybeDescendant)) {
    return false;
  }
  const auto* tree = registry.try_get<donner::components::TreeComponent>(maybeDescendant);
  while (tree != nullptr) {
    const Entity parent = tree->parent();
    if (parent == root) {
      return true;
    }
    if (parent == entt::null) {
      return false;
    }
    tree = registry.try_get<donner::components::TreeComponent>(parent);
  }
  return false;
}

/// True if @p entity is currently reachable from @p rootEntity by walking
/// TreeComponent parent links. The document root itself counts as
/// reachable. A detached entity (e.g. after `SVGElement::remove()`)
/// returns false: its `TreeComponent::parent_` is null and it's not
/// the root, so the walker bails immediately. Used to detect orphan
/// hints that the 30-frame demotion hysteresis would otherwise keep
/// in `activeHints_` after the user deletes a promoted element.
bool IsEntityInLiveTree(Registry& registry, Entity entity, Entity rootEntity) {
  if (entity == rootEntity) {
    return registry.valid(entity);
  }
  return IsDomDescendantOf(registry, entity, rootEntity);
}

ImageResource BuildImageResource(const RendererBitmap& bitmap) {
  ImageResource img;
  img.width = bitmap.dimensions.x;
  img.height = bitmap.dimensions.y;
  if (bitmap.alphaType == AlphaType::Premultiplied) {
    // ImageResource is interpreted as unpremultiplied RGBA by drawImage.
    img.data = UnpremultiplyRgba(bitmap.pixels);
  } else {
    img.data = bitmap.pixels;
  }
  return img;
}

Vector2i LayerPayloadDimensions(const CompositorLayer& layer) {
  if (layer.hasValidBitmap()) {
    return layer.bitmap().dimensions;
  }
  if (layer.textureSnapshot() != nullptr) {
    return layer.textureSnapshot()->dimensions();
  }
  return Vector2i::Zero();
}

struct LayerRasterGeometry {
  RenderViewport viewport;
  Transform2d surfaceFromCanvas;
  Vector2d canvasOffset = Vector2d::Zero();
  bool tight = false;
};

Vector2i BitmapDimensionsForViewport(const RenderViewport& viewport) {
  return Vector2i(static_cast<int>(viewport.size.x * viewport.devicePixelRatio),
                  static_cast<int>(viewport.size.y * viewport.devicePixelRatio));
}

LayerRasterGeometry ComputeLayerRasterGeometry(RendererInterface& renderer, Registry& registry,
                                               Entity firstEntity, Entity lastEntity,
                                               const RenderViewport& viewport) {
  LayerRasterGeometry result;
  result.viewport = viewport;

  // Intrinsic-size rasterization (design doc 0033 §M2): size the
  // offscreen to the layer's tight canvas bounds instead of the full
  // viewport. Editor-promoted layers go through this path too;
  // `CompositedPreview` carries the layer's `canvasOffset()` so the
  // editor blits the texture at its intrinsic dimensions + position
  // (see RenderPanePresenter). M2A scoped this to mandatory-detected
  // layers; M2B drops the gate.
  //
  // `computeEntityRangeBounds` already accounts for filter expansion,
  // stroke widths, isolated-layer accumulation, and clip rects — see
  // `RendererDriver.h §computeEntityRangeBounds`. `nullopt` means "fall
  // back to canvas-size"; never "empty".
  std::optional<Box2d> tightBoundsCanvas;
  {
    ZoneScopedN("Compositor::computeLayerRasterGeometry::computeBounds");
    RendererDriver boundsDriver(renderer);
    tightBoundsCanvas = boundsDriver.computeEntityRangeBounds(registry, firstEntity, lastEntity,
                                                              viewport, Transform2d());
  }

  // Snap to integer pixels and pad for AA. The padding matters: filter
  // primitives (gaussian blur in particular) produce a soft falloff outside the entity's
  // geometric bbox. `computeEntityRangeBounds` returns the filter region (per spec, a hard clip),
  // but the AA at its edge still has sub-pixel contributions that need a 1-2 px halo on either
  // side to stay pixel-identical with the canvas-size rasterize. 2 px is the smallest value that
  // survived `TightBoundedSegmentsSurviveExplicitDragTargetPromote` (a tightly packed scene with
  // two large gaussian blurs); 1 px left a ~0.001 alpha tail clipped at the bitmap edge, producing
  // maxDiff=1 in ~174 boundary pixels.
  constexpr double kEdgePaddingPx = 2.0;
  Box2d tightBoundsSnapped;
  if (tightBoundsCanvas.has_value()) {
    const Vector2d padding(kEdgePaddingPx, kEdgePaddingPx);
    Box2d padded(tightBoundsCanvas->topLeft - padding, tightBoundsCanvas->bottomRight + padding);
    const Vector2d snapTL(std::floor(std::max(0.0, padded.topLeft.x)),
                          std::floor(std::max(0.0, padded.topLeft.y)));
    const Vector2d snapBR(std::ceil(std::min(viewport.size.x, padded.bottomRight.x)),
                          std::ceil(std::min(viewport.size.y, padded.bottomRight.y)));
    if (snapBR.x > snapTL.x && snapBR.y > snapTL.y) {
      tightBoundsSnapped = Box2d(snapTL, snapBR);
      result.tight = tightBoundsSnapped.width() < viewport.size.x ||
                     tightBoundsSnapped.height() < viewport.size.y;
    }
  }

  if (result.tight) {
    result.viewport.size = tightBoundsSnapped.size();
    result.surfaceFromCanvas = Transform2d::Translate(-tightBoundsSnapped.topLeft);
    result.canvasOffset = tightBoundsSnapped.topLeft;
  }

  return result;
}

bool IsIntegerTranslation(const Transform2d& transform, Vector2d* roundedTranslation) {
  if (!transform.isTranslation()) {
    return false;
  }

  constexpr double kIntegerTolerance = 1e-6;
  const Vector2d translation = transform.translation();
  const Vector2d rounded(std::round(translation.x), std::round(translation.y));
  if (!NearEquals(translation.x, rounded.x, kIntegerTolerance) ||
      !NearEquals(translation.y, rounded.y, kIntegerTolerance)) {
    return false;
  }

  *roundedTranslation = rounded;
  return true;
}

}  // namespace

CompositorController::CompositorController(SVGDocument& document, RendererInterface& renderer,
                                           CompositorConfig config)
    : document_(document),
      renderer_(renderer),
      config_(config),
      // Only carve subtrees into bucket layers when they're actually
      // expensive to re-rasterize. `filterPenalty = 16` (default) plus the
      // subtree's own entity cost puts any filter group well above this
      // threshold, so filter-heavy subtrees still auto-bucket, while a lone
      // `<rect>` or two-element `<g>` stays in the root where it belongs.
      // `ComplexityBucketerConfig`'s docstring calls out the drawEntityRange
      // edge case that the `minCostToBucket = 1` default can expose; this
      // production value sidesteps it.
      complexityBucketer_(ComplexityBucketerConfig{.minCostToBucket = 5}) {}

CompositorController::~CompositorController() = default;

CompositorController::CompositorController(CompositorController&&) noexcept = default;
CompositorController& CompositorController::operator=(CompositorController&&) noexcept = default;

CompositorController::PromoteResult CompositorController::promoteEntity(
    Entity entity, InteractionHint interactionKind) {
  Registry& registry = document().registry();

  const auto refuse = [&](PromoteRefusalReason reason) {
    lastPromoteRefusalReason_ = reason;
    lastPromoteRefusalEntity_ = entity;
    switch (reason) {
      case PromoteRefusalReason::InvalidEntity: return PromoteResult{PromoteResult::InvalidEntity};
      case PromoteRefusalReason::LayerLimit: return PromoteResult{PromoteResult::LayerLimit};
      case PromoteRefusalReason::MemoryLimit: return PromoteResult{PromoteResult::MemoryLimit};
      case PromoteRefusalReason::DescendantPromoted:
        return PromoteResult{PromoteResult::DescendantPromoted};
      case PromoteRefusalReason::None:
        return PromoteResult{PromoteResult::FullCanvasPreviewRequired};
    }
    return PromoteResult{PromoteResult::FullCanvasPreviewRequired};
  };

  if (!registry.valid(entity)) {
    return refuse(PromoteRefusalReason::InvalidEntity);
  }

  // Descendants under an ancestor filter / mask / clip-path cannot be extracted into their own
  // layer without losing the ancestor compositing context. They are still presentable through the
  // compositor as a full-canvas tile.
  if (HasCompositingBreakingAncestor(registry, entity)) {
    lastPromoteRefusalReason_ = PromoteRefusalReason::None;
    lastPromoteRefusalEntity_ = entt::null;
    return PromoteResult{PromoteResult::FullCanvasPreviewRequired};
  }

  // Design doc 0033 §M9 — layer-set hysteresis. If `entity` is in the
  // pending-demotion queue, lift the demotion. The layer + hint
  // survived the `demoteEntity` call, so we can fall through to the
  // kind-refresh path below and reuse the cached bitmap / segment
  // split without any `resyncSegmentsToLayerSet` work. This is the
  // "click-deselect-click" / "drag-release-redrag-same-element" fast
  // path the milestone targets.
  pendingDemotions_.erase(entity);

  // Already promoted via the controller's `promoteEntity` path. Refresh the
  // Interaction kind in place so a Selection prewarm can become an ActiveDrag
  // without demoting the layer or dropping its cached bg/promoted/fg bitmaps.
  // Critical for drag-after-zoom: without this, the kind upgrade returned
  // early here, the compositor kept treating the entity as a Selection hint,
  // and the descendant-segment cascade marked unrelated segments dirty every
  // drag frame — turning a moderately-zoomed drag into 3-second slow frames.
  auto activeHintIt = activeHints_.find(entity);
  if (activeHintIt != activeHints_.end()) {
    if (config_.autoPromoteInteractions) {
      const std::optional<InteractionHint> activeKind = activeHintIt->second.interactionKind();
      if (!activeKind.has_value() || *activeKind != interactionKind) {
        activeHintIt->second.setInteractionKind(interactionKind);
      }
    }
    lastPromoteRefusalReason_ = PromoteRefusalReason::None;
    lastPromoteRefusalEntity_ = entt::null;
    return PromoteResult{PromoteResult::PromotedLayer};
  }

  if (activeHints_.size() >= static_cast<size_t>(kMaxCompositorLayers)) {
    return refuse(PromoteRefusalReason::LayerLimit);
  }

  if (totalBitmapMemory() >= kMaxCompositorMemoryBytes) {
    return refuse(PromoteRefusalReason::MemoryLimit);
  }

  // Refuse promotion when any descendant already has its own promoted
  // layer (non-zero `ComputedLayerAssignmentComponent::layerId`). A
  // user-promoted layer's range would span those descendants, causing
  // `drawEntityRange` to render them into this layer's bitmap AND into
  // the sub-layer's bitmap — double-exposed pixels on compose. This
  // manifested as crescent-shaped color drift at radial-gradient orb
  // edges when dragging `#Clouds_with_gradients` on the splash, which
  // contains cls-90 / cls-93 clip-path sublayers.
  //
  // Falling back to non-composited rendering for this specific drag
  // target costs compositor optimization but preserves correctness.
  // Typical interactive targets (single shapes, text letters, filter
  // groups themselves) are unaffected.
  {
    using TreeComponent = donner::components::TreeComponent;
    std::vector<Entity> stack;
    if (const auto* tree = registry.try_get<TreeComponent>(entity)) {
      for (Entity child = tree->firstChild(); child != entt::null;
           child = registry.get<TreeComponent>(child).nextSibling()) {
        stack.push_back(child);
      }
    }
    while (!stack.empty()) {
      const Entity descendant = stack.back();
      stack.pop_back();
      const auto* assignment = registry.try_get<ComputedLayerAssignmentComponent>(descendant);
      if (assignment != nullptr && assignment->layerId != 0) {
        return refuse(PromoteRefusalReason::DescendantPromoted);
      }
      if (const auto* descTree = registry.try_get<TreeComponent>(descendant)) {
        for (Entity grandchild = descTree->firstChild(); grandchild != entt::null;
             grandchild = registry.get<TreeComponent>(grandchild).nextSibling()) {
          stack.push_back(grandchild);
        }
      }
    }
  }

  // Phase 2: under `autoPromoteInteractions`, the editor-driven
  // `promoteEntity` call publishes an `Interaction` hint tagged with the
  // caller-supplied kind (`Selection` for pre-warm on selection,
  // `ActiveDrag` for an in-flight drag). When the gate is off, we fall back
  // to the `Explicit` escape hatch. Either way the hint is tracked in
  // `activeHints_` and `isPromoted` returns true.
  ScopedCompositorHint hint =
      config_.autoPromoteInteractions
          ? ScopedCompositorHint::Interaction(registry, entity, interactionKind)
          : ScopedCompositorHint::Explicit(registry, entity);
  const auto [it, inserted] = activeHints_.try_emplace(entity, std::move(hint));
  UTILS_RELEASE_ASSERT(inserted);
  static_cast<void>(it);

  resolver_.resolve(registry, kMaxCompositorLayers);
  reconcileLayers(registry);

  const auto* assignment = registry.try_get<ComputedLayerAssignmentComponent>(entity);
  if (assignment == nullptr || assignment->layerId == 0) {
    activeHints_.erase(entity);
    resolver_.resolve(registry, kMaxCompositorLayers);
    reconcileLayers(registry);
    // The resolver refused the assignment — treat as "descendant
    // promoted" since that's the only realistic cause of post-
    // emplace resolver rejection (the explicit hint exists, but the
    // resolver picked a different overlapping promote that owns the
    // layer slot).
    return refuse(PromoteRefusalReason::DescendantPromoted);
  }

  // Don't force a full cache invalidation — `resyncSegmentsToLayerSet`
  // preserves segments whose boundary identity survives the new layer
  // insertion, which is what keeps click-to-first-pixel fast on the
  // splash's first drag-target promote.
  lastPromoteRefusalReason_ = PromoteRefusalReason::None;
  lastPromoteRefusalEntity_ = entt::null;
  return PromoteResult{PromoteResult::PromotedLayer};
}

void CompositorController::demoteEntity(Entity entity) {
  // Design doc 0033 §M9 — layer-set hysteresis. Don't run the
  // resolver / reconcileLayers immediately; queue the demotion
  // against a frame counter and let the layer + hint linger.
  // `promoteEntity` for the same entity within the window cancels
  // the queued demotion and reuses the cached bitmap (no
  // `resyncSegmentsToLayerSet` churn). Once the counter expires,
  // `processPendingDemotions` runs the deferred cleanup in a batch.
  //
  // Only entities currently in `activeHints_` qualify. Mandatory-
  // detector hints / complexity-bucket hints live in separate maps
  // and aren't part of the explicit promote/demote API, so calling
  // `demoteEntity` for those is a silent no-op (matching the
  // pre-§M9 behaviour, where the `activeHints_.find` miss was also
  // a no-op).
  if (!activeHints_.contains(entity)) {
    return;
  }

  // Hysteresis only makes sense for live entities that can be promoted again.
  // Detached entities must leave `activeHints_` immediately so stale
  // paint-order ranges cannot survive deletion.
  Registry& registry = document().registry();
  const Entity rootEntity = document().svgElement().entityHandle().entity();
  if (!IsEntityInLiveTree(registry, entity, rootEntity)) {
    activeHints_.erase(entity);
    pendingDemotions_.erase(entity);
    if (splitStaticLayersEntity_ == entity) {
      splitStaticLayersEntity_ = entt::null;
      splitStaticLayersViewport_ = Vector2i::Zero();
    }
    return;
  }

  pendingDemotions_[entity] = kDemotionHysteresisFrames;

  // The split bg / drag / fg cache is keyed on the editor's current
  // drag entity. Even though the layer lives on through the hysteresis
  // window, the editor's view of "this entity is the drag target" is
  // over — clear `splitStaticLayersEntity_` so the next
  // `snapshotTilesForUpload` doesn't mark the about-to-be-demoted
  // layer with `isDragTarget=true`.
  splitStaticLayersEntity_ = entt::null;
  splitStaticLayersViewport_ = Vector2i::Zero();
}

void CompositorController::flushPendingDemotionsForTesting() {
  if (pendingDemotions_.empty()) {
    return;
  }
  // Drop the per-entry counters to zero so the next processing pass
  // expires all of them. We can't just `clear()` + erase from
  // `activeHints_` because the deferred resolver / reconcile work
  // still needs to run — push it through the normal path.
  for (auto& [entity, frames] : pendingDemotions_) {
    frames = 0;
  }
  processPendingDemotions(document().registry());
}

void CompositorController::processPendingDemotions(Registry& registry) {
  if (pendingDemotions_.empty()) {
    return;
  }

  std::vector<Entity> expired;
  for (auto it = pendingDemotions_.begin(); it != pendingDemotions_.end();) {
    if (it->second > 0) {
      --it->second;
      ++it;
    } else {
      expired.push_back(it->first);
      it = pendingDemotions_.erase(it);
    }
  }
  // Decrement-then-act: an entry minted this frame (count =
  // `kDemotionHysteresisFrames`) needs `kDemotionHysteresisFrames`
  // calls to this function before it fires. The `> 0` check above
  // means a fresh entry is decremented to `kDemotionHysteresisFrames
  // - 1` on the first call and only enters the `expired` bucket
  // when it would otherwise tick below zero.
  if (expired.empty()) {
    return;
  }

  for (Entity entity : expired) {
    activeHints_.erase(entity);
  }
  // Batch a single resolver + reconcile pass for all expirations
  // — the loop above might have removed several hints at once, and
  // running the resolver per-entity would N² the segment cache
  // rebuild. `resyncSegmentsToLayerSet` (called from
  // `renderFrame`'s normal flow later this tick) preserves every
  // segment whose boundary pair survived the layer removals.
  resolver_.resolve(registry, kMaxCompositorLayers);
  reconcileLayers(registry);
}

bool CompositorController::dropNonRenderableInteractionHints(Registry& registry) {
  std::vector<Entity> droppedEntities;
  droppedEntities.reserve(activeHints_.size());
  for (const auto& [entity, hint] : activeHints_) {
    (void)hint;
    if (!registry.valid(entity) ||
        !registry.all_of<components::RenderingInstanceComponent>(entity)) {
      droppedEntities.push_back(entity);
    }
  }

  if (droppedEntities.empty()) {
    return false;
  }

  for (const Entity entity : droppedEntities) {
    activeHints_.erase(entity);
    pendingDemotions_.erase(entity);
    if (splitStaticLayersEntity_ == entity) {
      splitStaticLayersEntity_ = entt::null;
      splitStaticLayersViewport_ = Vector2i::Zero();
    }
  }

  return true;
}

bool CompositorController::isPromoted(Entity entity) const {
  return activeHints_.contains(entity);
}

Transform2d CompositorController::layerComposeOffset(Entity entity) const {
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->canvasFromBitmap() : Transform2d();
}

FallbackReason CompositorController::fallbackReasonsOf(Entity entity) const {
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->fallbackReasons() : FallbackReason::None;
}

namespace {

/// Nearest-neighbor downsample of an RGBA8 bitmap into a thumbnail of at
/// most `kLayerThumbnailMaxSide` on the longer side, preserving aspect.
/// Output is tightly packed (no row padding) so the editor can hand it
/// directly to `glTexImage2D`.
void BuildThumbnail(const RendererBitmap& bitmap, Vector2i* outDims,
                    std::vector<uint8_t>* outPixels) {
  *outDims = Vector2i::Zero();
  outPixels->clear();

  if (bitmap.empty()) {
    return;
  }

  const int srcW = bitmap.dimensions.x;
  const int srcH = bitmap.dimensions.y;
  const int longSide = std::max(srcW, srcH);
  const int maxSide = CompositorController::kLayerThumbnailMaxSide;

  int thumbW;
  int thumbH;
  if (longSide <= maxSide) {
    thumbW = srcW;
    thumbH = srcH;
  } else {
    const double scale = static_cast<double>(maxSide) / static_cast<double>(longSide);
    thumbW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
    thumbH = std::max(1, static_cast<int>(std::lround(srcH * scale)));
  }

  outDims->x = thumbW;
  outDims->y = thumbH;
  outPixels->resize(static_cast<size_t>(thumbW) * static_cast<size_t>(thumbH) * 4u);

  const size_t srcRowBytes =
      bitmap.rowBytes != 0u ? bitmap.rowBytes : static_cast<size_t>(srcW) * 4u;

  for (int y = 0; y < thumbH; ++y) {
    const int srcY = std::min(srcH - 1, (y * srcH) / thumbH);
    const uint8_t* srcRow = bitmap.pixels.data() + static_cast<size_t>(srcY) * srcRowBytes;
    uint8_t* dstRow = outPixels->data() + static_cast<size_t>(y) * static_cast<size_t>(thumbW) * 4u;
    for (int x = 0; x < thumbW; ++x) {
      const int srcX = std::min(srcW - 1, (x * srcW) / thumbW);
      const uint8_t* srcPx = srcRow + static_cast<size_t>(srcX) * 4u;
      uint8_t* dstPx = dstRow + static_cast<size_t>(x) * 4u;
      dstPx[0] = srcPx[0];
      dstPx[1] = srcPx[1];
      dstPx[2] = srcPx[2];
      dstPx[3] = srcPx[3];
    }
  }
}

bool IsTransparentPlaceholderBitmap(const RendererBitmap& bitmap) {
  return bitmap.dimensions == Vector2i(1, 1) && bitmap.pixels.size() == 4u &&
         std::all_of(bitmap.pixels.begin(), bitmap.pixels.end(),
                     [](uint8_t channel) { return channel == 0u; });
}

// Empty paint-order gaps are cached internally as transparent 1x1 bitmaps so
// `RendererBitmap::empty()` can keep meaning "not rasterized yet". Public tile
// snapshots and composition should ignore those bookkeeping placeholders.
bool HasPublicTileBitmap(const RendererBitmap& bitmap) {
  return !bitmap.empty() && !IsTransparentPlaceholderBitmap(bitmap);
}

}  // namespace

std::vector<CompositorController::CompositeTileSnapshot>
CompositorController::snapshotCompositeTiles() const {
  std::vector<CompositeTileSnapshot> tiles;

  const auto pushPayloadTile = [&tiles](
                                   CompositeTileSnapshot::Kind kind, std::string id,
                                   std::string label, const RendererBitmap& bitmap,
                                   const std::shared_ptr<const RendererTextureSnapshot>& texture,
                                   uint64_t generation, double lastRasterizeMs, bool isDragTarget) {
    CompositeTileSnapshot tile;
    tile.kind = kind;
    tile.id = std::move(id);
    tile.label = std::move(label);
    tile.generation = generation;
    tile.lastRasterizeMs = lastRasterizeMs;
    tile.isDragTarget = isDragTarget;
    tile.hasValidBitmap = !bitmap.empty() || texture != nullptr;
    if (tile.hasValidBitmap) {
      tile.bitmapDims = !bitmap.empty() ? bitmap.dimensions : texture->dimensions();
      if (!bitmap.empty()) {
        BuildThumbnail(bitmap, &tile.thumbnailDims, &tile.thumbnailPixels);
      }
      tile.textureSnapshot = texture;
    }
    tiles.push_back(std::move(tile));
  };

  // Always emit the full segments+layers paint-order breakdown — even
  // when the editor's split-bitmap optimization is active. User
  // feedback on commit `74083723`: "we just split the current segment
  // into three layers and keep the other tiles surrounding it
  // unmodified" — showing only `bg/drag/fg` collapses the source-of-
  // truth structure and hides which sibling layers the drag target
  // sits among. Highlight the drag target inline; append the composed
  // bg/fg below as auxiliary views.
  const size_t layerCount = layers_.size();
  for (size_t i = 0; i <= layerCount; ++i) {
    const RendererBitmap* segmentBitmap =
        (i < staticSegments_.size() && HasPublicTileBitmap(staticSegments_[i]))
            ? &staticSegments_[i]
            : nullptr;
    const std::shared_ptr<const RendererTextureSnapshot> segmentTexture =
        i < staticSegmentTextures_.size() ? staticSegmentTextures_[i] : nullptr;
    if (segmentBitmap != nullptr || segmentTexture != nullptr) {
      char label[32];
      std::snprintf(label, sizeof(label), "segment %zu", i);
      char id[32];
      std::snprintf(id, sizeof(id), "seg:%zu", i);
      const uint64_t segGen =
          i < staticSegmentGeneration_.size() ? staticSegmentGeneration_[i] : 0u;
      const double segMs =
          i < staticSegmentLastRasterizeMs_.size() ? staticSegmentLastRasterizeMs_[i] : 0.0;
      pushPayloadTile(CompositeTileSnapshot::Kind::Segment, id, label,
                      segmentBitmap != nullptr ? *segmentBitmap : RendererBitmap{}, segmentTexture,
                      segGen, segMs, /*isDragTarget=*/false);
    }
    if (i < layerCount) {
      const CompositorLayer& layer = layers_[i];
      const bool isDragTarget = layer.entity() == splitStaticLayersEntity_;
      char label[64];
      if (isDragTarget) {
        std::snprintf(label, sizeof(label), "layer #%u (drag)",
                      static_cast<unsigned>(layer.entity()));
      } else {
        std::snprintf(label, sizeof(label), "layer #%u", static_cast<unsigned>(layer.entity()));
      }
      char id[48];
      std::snprintf(id, sizeof(id), "layer:%u", static_cast<unsigned>(layer.entity()));
      pushPayloadTile(CompositeTileSnapshot::Kind::Layer, id, label, layer.bitmap(),
                      layer.textureSnapshot(), layer.generation(), layer.lastRasterizeMs(),
                      isDragTarget);
    }
  }

  // Post-§M2C: bg/fg tiles no longer exist (the compositor doesn't
  // pre-flatten — the editor blits segments and layers directly).
  // The Kind::Background / Kind::Foreground enum values are retained
  // for source compatibility but `snapshotCompositeTiles` no longer
  // emits them.
  return tiles;
}

CompositorController::StateSnapshot CompositorController::snapshotState() const {
  StateSnapshot out;
  out.activeHintsCount = static_cast<uint32_t>(activeHints_.size());
  out.layerCount = static_cast<uint32_t>(layers_.size());
  out.splitPathActive = hasSplitStaticLayers();
  out.splitStaticLayersEntity = splitStaticLayersEntity_;
  out.canvasSize = staticSegmentsCanvas_;
  out.lastPromoteRefusalReason = lastPromoteRefusalReason_;
  out.lastPromoteRefusalEntity = lastPromoteRefusalEntity_;
  return out;
}

std::vector<CompositorController::SegmentInspectorRow>
CompositorController::snapshotSegmentInspectorRows() const {
  const size_t count = staticSegments_.size();
  std::vector<SegmentInspectorRow> rows;
  rows.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    SegmentInspectorRow row;
    row.slotIndex = i;
    const RendererBitmap& bitmap = staticSegments_[i];
    const std::shared_ptr<const RendererTextureSnapshot> texture =
        i < staticSegmentTextures_.size() ? staticSegmentTextures_[i] : nullptr;
    row.hasValidBitmap = !bitmap.empty() || texture != nullptr;
    if (!bitmap.empty()) {
      row.bitmapSize = bitmap.dimensions;
    } else if (texture != nullptr) {
      row.bitmapSize = texture->dimensions();
    }
    if (i < staticSegmentOffsets_.size()) {
      row.canvasOffset = staticSegmentOffsets_[i];
    }
    if (i < staticSegmentGeneration_.size()) {
      row.generation = staticSegmentGeneration_[i];
    }
    if (i < staticSegmentLastRasterizeMs_.size()) {
      row.lastRasterizeMs = staticSegmentLastRasterizeMs_[i];
    }
    if (i < staticSegmentDirty_.size()) {
      row.dirty = staticSegmentDirty_[i];
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<CompositorController::LayerInspectorRow>
CompositorController::snapshotLayerInspectorRows() const {
  std::vector<LayerInspectorRow> rows;
  rows.reserve(layers_.size());
  for (const CompositorLayer& layer : layers_) {
    LayerInspectorRow row;
    row.layerId = layer.id();
    row.entity = layer.entity();
    if (layer.hasValidBitmap()) {
      row.bitmapSize = layer.bitmap().dimensions;
      BuildThumbnail(layer.bitmap(), &row.thumbnailDims, &row.thumbnailPixels);
    } else if (layer.textureSnapshot() != nullptr) {
      row.bitmapSize = layer.textureSnapshot()->dimensions();
    }
    row.generation = layer.generation();
    row.rasterizeCount = layer.rasterizeCount();
    row.lastRasterizeMs = layer.lastRasterizeMs();
    row.dirty = layer.isDirty();
    row.hasValidBitmap = layer.hasRenderablePayload();
    row.fallbackReasons = layer.fallbackReasons();
    row.fallbackReasonsText = FallbackReasonToString(layer.fallbackReasons());
    row.canvasOffset = layer.canvasOffset();
    rows.push_back(std::move(row));
  }
  return rows;
}

bool CompositorController::hasSplitStaticLayers() const {
  // Post-design-doc 0033 §M2C: the editor blits segments + layers
  // directly, no bg/fg flatten step exists anymore. "Split static
  // layers active" now means "there's exactly one editor-promoted
  // entity (the drag target) with a rasterized layer the editor can
  // upload". Bucket layers stay static during a drag and don't
  // invalidate the split, so we check `activeHints_` (explicit
  // promotions) rather than `layers_.size()`.
  //
  // §M9 wrinkle: pending-demote entries linger in `activeHints_` for
  // the hysteresis window. Counting them would mean a selection-
  // change drag (demote old → promote new) makes this return false
  // for ~30 renderFrames, disabling the `skipMainCompose`
  // optimization and forcing `composeLayers` to do 2N+1 bitmap blits
  // every fast-path drag frame at canvas scale — visible to the
  // operator as "fast path counter bumps but framerate stays low,
  // worse at higher zoom". Match `splitStaticLayersEntity_`'s
  // carve-out: count only entries NOT pending demotion.
  uint32_t liveHints = 0;
  Entity liveCandidate = entt::null;
  for (const auto& [hintEntity, hint] : activeHints_) {
    if (pendingDemotions_.contains(hintEntity)) {
      continue;
    }
    ++liveHints;
    liveCandidate = hintEntity;
  }
  if (liveHints != 1) {
    return false;
  }
  const CompositorLayer* layer = findLayer(liveCandidate);
  return layer != nullptr && layer->hasRenderablePayload();
}

const RendererBitmap& CompositorController::layerBitmapOf(Entity entity) const {
  static const RendererBitmap kEmptyBitmap;
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->bitmap() : kEmptyBitmap;
}

Vector2d CompositorController::layerCanvasOffsetOf(Entity entity) const {
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->canvasOffset() : Vector2d::Zero();
}

std::unordered_map<Entity, Entity> BuildStructuralEntityRemap(const SVGDocument& oldDoc,
                                                              const SVGDocument& newDoc) {
  std::unordered_map<Entity, Entity> remap;

  // Recursive lockstep walk. `fail` short-circuits the walk on any
  // mismatch; callers check for an empty return map.
  struct Walker {
    std::unordered_map<Entity, Entity>& remap;
    bool fail = false;

    void step(SVGElement oldEl, SVGElement newEl) {
      if (fail) return;
      // Tag name must match byte-for-byte (namespace-qualified).
      if (oldEl.tagName() != newEl.tagName()) {
        fail = true;
        return;
      }
      // `id` attribute must match. Missing id on both is fine.
      if (oldEl.id() != newEl.id()) {
        fail = true;
        return;
      }
      // Record this mapping (even for elements without compositor state
      // — they may be ancestors of layer ranges).
      remap[oldEl.entityHandle().entity()] = newEl.entityHandle().entity();

      // Walk children in lockstep.
      auto oldChild = oldEl.firstChild();
      auto newChild = newEl.firstChild();
      while (oldChild.has_value() && newChild.has_value() && !fail) {
        step(*oldChild, *newChild);
        oldChild = oldChild->nextSibling();
        newChild = newChild->nextSibling();
      }
      if (oldChild.has_value() != newChild.has_value()) {
        // Different number of children at this level.
        fail = true;
      }
    }
  };

  Walker walker{remap};
  walker.step(oldDoc.svgElement(), newDoc.svgElement());
  if (walker.fail) {
    remap.clear();
  }
  return remap;
}

bool CompositorController::remapAfterStructuralReplace(
    const std::unordered_map<Entity, Entity>& remap) {
  Registry& registry = document().registry();

  // Prepare the new document FIRST. `prepareDocumentForRendering` builds
  // all the component storages (`RenderingInstanceComponent`, computed
  // style caches, etc.) for the new entity space. Skipping this and
  // trying to `get_or_emplace<CompositorHintComponent>` on the bare
  // post-swap registry trips an entt assertion in `dense_map::fast_mod`
  // — the new registry's internal storage hasn't been warmed, so the
  // power-of-two capacity invariant isn't established yet. Preparing
  // the doc fixes that, plus sets `documentPrepared_=true` so the
  // post-remap `renderFrame` skips redundant prepare work.
  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document(), /*verbose=*/false, warningSink);
  documentPrepared_ = true;
  if (registry.ctx().contains<components::RenderTreeState>()) {
    registry.ctx().get<components::RenderTreeState>().needsFullRebuild = false;
  }

  // Step 1: rebuild `activeHints_` against the new entity space. The
  // underlying `ScopedCompositorHint`'s `registry_` pointer is still
  // valid (same storage address), but its `entity_` id is from the
  // old entity space; `remapToNewEntity` re-emplaces the hint on the
  // new entity without touching the stale old id.
  std::unordered_map<Entity, ScopedCompositorHint> newActiveHints;
  newActiveHints.reserve(activeHints_.size());
  for (auto& [oldEntity, hint] : activeHints_) {
    const auto it = remap.find(oldEntity);
    if (it == remap.end()) {
      return false;
    }
    hint.remapToNewEntity(registry, it->second);
    newActiveHints.emplace(it->second, std::move(hint));
  }
  activeHints_ = std::move(newActiveHints);

  // §M9: remap the hysteresis queue alongside `activeHints_`. Entries
  // whose entity id doesn't survive the remap are dropped — the layer
  // they were guarding is already gone from the new entity space, so
  // there's nothing left to demote later.
  std::unordered_map<Entity, uint32_t> newPendingDemotions;
  newPendingDemotions.reserve(pendingDemotions_.size());
  for (const auto& [oldEntity, framesRemaining] : pendingDemotions_) {
    const auto it = remap.find(oldEntity);
    if (it != remap.end()) {
      newPendingDemotions.emplace(it->second, framesRemaining);
    }
  }
  pendingDemotions_ = std::move(newPendingDemotions);

  // Step 2: rebuild the auto-promotion detectors against the new
  // registry. `prepareDocumentForRendering` above populated RICs, so
  // `reconcile` has the data it needs to re-detect filter / bucket
  // layers and publish fresh hints keyed on the new entity ids.
  mandatoryDetector_.rebuildForReplacedDocument(registry);
  if (config_.complexityBucketing) {
    complexityBucketer_.rebuildForReplacedDocument(registry);
  }
  hintsScanned_ = true;
  const bool droppedNonRenderableHints = dropNonRenderableInteractionHints(registry);

  // Step 3: remap layer entity ids. Cached `bitmap_`, `canvasFromBitmap_`,
  // `bitmapEntityFromWorldTransform_`, and `fallbackReasons_` survive
  // initially because they are keyed on the paint-order slot. After
  // reconcile refreshes assignments, the validation pass below dirties
  // interaction layers whose preserved bitmap is no longer pixel-exact
  // for the reparsed DOM.
  for (auto& layer : layers_) {
    const auto eIt = remap.find(layer.entity());
    const auto firstIt = remap.find(layer.firstEntity());
    const auto lastIt = remap.find(layer.lastEntity());
    if (eIt == remap.end() || firstIt == remap.end() || lastIt == remap.end()) {
      return false;
    }
    layer.remapEntities(eIt->second, firstIt->second, lastIt->second);
  }

  // Step 4: flush ancillary entity references. `splitStaticLayersEntity_`
  // keys the bg/fg cache; remap it or clear if unmappable.
  if (splitStaticLayersEntity_ != entt::null) {
    const auto it = remap.find(splitStaticLayersEntity_);
    if (it != remap.end()) {
      splitStaticLayersEntity_ = it->second;
    } else {
      // Cache identity lost — drop the split-target id so the next
      // `snapshotTilesForUpload` re-derives it from `activeHints_`.
      splitStaticLayersEntity_ = entt::null;
    }
  }

  // Re-resolve and reconcile so `ComputedLayerAssignmentComponent`s
  // land on the new entities. `reconcileLayers` iterates the layer
  // assignment view and updates/adds layers; my `findLayer` lookup
  // (keyed on `layer.entity()` which we just remapped to new ids)
  // lets it find the existing layer entries and keep their cached
  // bitmaps.
  const ResolveOptions resolveOptions{
      .enableInteractionHints = config_.autoPromoteInteractions,
      .enableAnimationHints = config_.autoPromoteAnimations,
      .enableComplexityBucketHints = config_.complexityBucketing,
  };
  resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
  reconcileLayers(registry);
  if (droppedNonRenderableHints) {
    markAllSegmentsDirty();
  }

  // The remap proves the tree shape survived, not that cached layer pixels
  // are still a pixel-exact final render. Drag writeback commonly changes
  // only a `transform` attribute: during the live drag, shifting a cached
  // bitmap is fine for interactivity, but after the DOM has been reparsed the
  // settled render must match a fresh vector raster. A non-integer
  // translation, a non-translation delta, or a changed tight raster rectangle
  // requires re-rasterizing just that layer. Limit this to interaction
  // layers: mandatory / bucket layers do not carry the editor's live drag
  // delta, and their cache validity is already governed by normal dirty
  // flags. Their resolved metadata can shift during prepare even when their
  // pixel cache is still reusable, so validating them here would throw away
  // unrelated filter caches on every source writeback.
  const RenderViewport viewport = hasLastViewport_ ? lastViewport_
                                                   : RenderViewport{
                                                         .size = Vector2d(document().canvasSize()),
                                                         .devicePixelRatio = 1.0,
                                                     };
  for (auto& layer : layers_) {
    if (layer.isDirty() || !layer.hasRenderablePayload()) {
      continue;
    }
    if (!activeHints_.contains(layer.entity())) {
      continue;
    }

    bool cacheStillValid = false;
    if (layer.bitmapEntityFromWorldTransform().has_value() &&
        registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
      const auto& instance = registry.get<components::RenderingInstanceComponent>(layer.entity());
      const Transform2d delta =
          layer.bitmapEntityFromWorldTransform()->inverse() * instance.worldFromEntityTransform;

      Vector2d roundedTranslation;
      if (IsIntegerTranslation(delta, &roundedTranslation)) {
        const LayerRasterGeometry geometry = ComputeLayerRasterGeometry(
            renderer(), registry, layer.firstEntity(), layer.lastEntity(), viewport);
        const Vector2i expectedBitmapDims = BitmapDimensionsForViewport(geometry.viewport);
        const Vector2d composedCanvasOffset = layer.canvasOffset() + roundedTranslation;
        cacheStillValid = LayerPayloadDimensions(layer) == expectedBitmapDims &&
                          (!geometry.tight || composedCanvasOffset == geometry.canvasOffset);

        if (cacheStillValid) {
          layer.setCanvasFromBitmap(Transform2d::Translate(roundedTranslation));
        }
      }
    }

    if (!cacheStillValid) {
      layer.markDirty();
    }
  }

  refreshLayerMetadata();

  return true;
}

void CompositorController::resetAllLayers(bool documentReplaced) {
  Registry& registry = document().registry();
  if (documentReplaced) {
    // Old Registry was destroyed-and-reconstructed in place; the hints'
    // cached `Registry*` points at a live object that knows nothing about
    // the old entity IDs. Defuse every hint before clearing so the dtors
    // don't dereference into entt sparse-set pages that don't exist in the
    // new registry. The old `CompositorHintComponent`s went down with the
    // old entity space, so there's nothing to clean up.
    for (auto& [entity, hint] : activeHints_) {
      hint.release();
    }
    mandatoryDetector_.releaseAllHintsNoClean();
    complexityBucketer_.releaseAllHintsNoClean();
  } else {
    // Registry is still live — run the normal RAII cleanup so the resolver
    // can see the hints disappear and strip orphan `ComputedLayerAssignment
    // Component`s.
    mandatoryDetector_.clear();
    complexityBucketer_.clear();
  }
  activeHints_.clear();
  // §M9 hysteresis queue is tied to the old entity space; both
  // document-replaced and live-registry resets must drop it.
  pendingDemotions_.clear();
  resolver_.resolve(registry, kMaxCompositorLayers);
  reconcileLayers(registry);

  staticSegments_.clear();
  staticSegmentTextures_.clear();
  staticSegmentBoundaries_.clear();
  staticSegmentDirty_.clear();
  staticSegmentGeneration_.clear();
  staticSegmentLastRasterizeMs_.clear();
  staticSegmentOffsets_.clear();
  staticSegmentsCanvas_ = Vector2i::Zero();
  staticSegmentsLayerCount_ = 0;
  splitStaticLayersEntity_ = entt::null;
  splitStaticLayersViewport_ = Vector2i::Zero();
  rootDirty_ = true;
  documentPrepared_ = false;
  hintsScanned_ = false;
  // Main renderer's cached frame is for the old document — invalidate
  // so the next `composeLayers` does a full first-frame compose.
  mainRendererHasCachedFrame_ = false;
}

void CompositorController::setTightBoundedSegmentsEnabled(bool enabled) {
  if (config_.tightBoundedSegments == enabled) {
    return;
  }
  config_.tightBoundedSegments = enabled;
  // Existing cached segments were rasterized under the previous policy
  // — a segment rasterized tight has a non-zero offset that the
  // full-canvas path would mis-apply on compose, and vice versa. Mark
  // every slot dirty so the next frame rebuilds under the new policy.
  markAllSegmentsDirty();
}

bool CompositorController::renderFrame(const RenderViewport& viewport, CancellationToken& token) {
  // §M4: stash the token where the rasterize loops can poll it via
  // `isCancelled()`. The reference is active only for this render attempt.
  cancelToken_.emplace(token);
  renderFrame(viewport);
  const bool cancelled = token.isCancelled();
  cancelToken_.reset();
  return !cancelled;
}

void CompositorController::renderFrame(const RenderViewport& viewport) {
  ZoneScopedN("Compositor::renderFrame");

  lastViewport_ = viewport;
  hasLastViewport_ = true;
  Registry& registry = document().registry();

  // Design doc 0033 §M9 — age the hysteresis queue and flush
  // expirations before the dirty-flag snapshot. An entity that
  // expires this frame becomes a real demote whose
  // `resyncSegmentsToLayerSet` runs as part of the normal render
  // flow below; an entity that survives the frame stays in
  // `activeHints_` / `layers_` so its cached bitmap is reused.
  processPendingDemotions(registry);

  // `mandatoryDetector_.reconcile()` is O(N) over all `RenderingInstanceComponent`
  // entities. For steady-state drag (translation-only, no style mutations) it
  // produces the same hints frame after frame, so skip the walk unless the
  // document actually changed. First frame always runs (documentPrepared_ is
  // false); subsequent frames only run when something is dirty or a full
  // rebuild is pending. This keeps the per-frame drag cost O(hints), not O(N).
  // Snapshot dirty entities before the detectors / prepare step run:
  // `prepareDocumentForRendering` clears `DirtyFlagsComponent` as a side
  // effect, so reading the view later (in `consumeDirtyFlags`) would find
  // an empty set. With the snapshot we can still do precise per-layer
  // invalidation after the prepare.
  std::vector<Entity> dirtyEntitySnapshot;
  std::vector<Entity> transformDirtyEntities;
  {
    auto dirtyView = registry.view<components::DirtyFlagsComponent>();
    for (const Entity entity : dirtyView) {
      const auto& dirty = dirtyView.get<components::DirtyFlagsComponent>(entity);
      if (dirty.flags != components::DirtyFlagsComponent::Flags::None) {
        dirtyEntitySnapshot.push_back(entity);
        if ((dirty.flags & (components::DirtyFlagsComponent::Transform |
                            components::DirtyFlagsComponent::WorldTransform)) != 0) {
          transformDirtyEntities.push_back(entity);
        }
      }
    }
  }
  const bool anyEntityDirty = !dirtyEntitySnapshot.empty();
  const bool needsFullRebuild = registry.ctx().contains<components::RenderTreeState>() &&
                                registry.ctx().get<components::RenderTreeState>().needsFullRebuild;
  // `documentDirty` gets flipped to false further down if the fast-path
  // handles the dirty set surgically, so that the subsequent prepare /
  // detector / rasterize steps take the "nothing changed" branch.
  bool documentDirty = anyEntityDirty || needsFullRebuild;

  // Fast path: the drag-release `SetTransformCommand` mutation flips the
  // dragged entity's `DirtyFlagsComponent` with only position-related bits
  // (Layout / Transform / WorldTransform — no Style / Paint / Filter /
  // geometry). The default `prepareDocumentForRendering` handles this by
  // tearing down every `ComputedShadowTreeComponent`, every RIC, and every
  // `ComputedClipPathsComponent`, then recomputing styles / paint servers
  // / filters across the whole tree — O(N) for the splash that's ~200ms of
  // blocked UI, which is what the user saw as a "2s hang on mouse release"
  // (compounded by any queued prewarm / settle renders).
  //
  // When the only dirty entities are single-entity promoted layer roots
  // and their dirty flags are pure-position, we can skip the full tree
  // rebuild entirely: just refresh their `worldFromEntityTransform` from
  // the freshly-computed `ComputedAbsoluteTransformComponent` (invalidated
  // in place by `LayoutSystem::setTransform`) and mark the layer dirty.
  // Everything else — filter layer bitmaps, bg/fg split, style cascade —
  // stays untouched across the mutation.
  if (dirtyEntitySnapshot.empty() && !needsFullRebuild) {
    ++fastPathCounters_.noDirtyFrames;
  }
  bool fastPathTakenThisFrame = false;
  if (documentPrepared_ && !needsFullRebuild && !dirtyEntitySnapshot.empty() && hintsScanned_) {
    constexpr uint16_t kTransformOnlyMask = components::DirtyFlagsComponent::Layout |
                                            components::DirtyFlagsComponent::Transform |
                                            components::DirtyFlagsComponent::WorldTransform |
                                            components::DirtyFlagsComponent::RenderInstance;

    // Pre-compute per-entity resolution so we can validate ALL dirty
    // entities before mutating any RIC or layer state. If any check fails
    // the whole block bails out cleanly and the slow path
    // (`prepareDocumentForRendering`) takes over.
    struct FastPathResolution {
      Entity entity = entt::null;
      CompositorLayer* layer = nullptr;
      Transform2d newWorldFromEntity;
      /// `bitmapEntity_from_entity` — the canvas-from-canvas mapping from
      /// the bitmap's stamped entity frame to the entity's CURRENT frame.
      /// Used as the layer's `canvasFromBitmap` compose offset on the
      /// bitmap-reuse fast path. **Stamp-relative**, NOT per-frame.
      Transform2d bitmapEntityFromEntity;
      bool isSubtree = false;
    };
    std::vector<FastPathResolution> resolutions;
    resolutions.reserve(dirtyEntitySnapshot.size());

    bool eligible = true;
    for (Entity e : dirtyEntitySnapshot) {
      const auto* dirty = registry.try_get<components::DirtyFlagsComponent>(e);
      if (dirty == nullptr) continue;
      if ((dirty->flags & ~kTransformOnlyMask) != 0) {
        eligible = false;
        break;
      }
      if (!registry.all_of<components::RenderingInstanceComponent>(e)) {
        eligible = false;
        break;
      }

      // A promoted layer root owns the stamp transform used by the
      // translation-only fast path. Subtree layers are eligible; descendants
      // receive the per-frame translation after the root is updated.
      CompositorLayer* matchedLayer = nullptr;
      for (auto& layer : layers_) {
        if (layer.entity() == e) {
          matchedLayer = &layer;
          break;
        }
      }
      if (matchedLayer == nullptr) {
        // Transform writebacks dirty descendants of the promoted root. A
        // contained descendant does not disqualify the fast path because the
        // root's resolution carries the translation for the whole subtree.
        bool containedInPromotedLayer = false;
        for (auto& layer : layers_) {
          if (layerContainsEntity(layer, e)) {
            containedInPromotedLayer = true;
            break;
          }
        }
        if (containedInPromotedLayer) {
          continue;
        }
        eligible = false;
        break;
      }
      if (!matchedLayer->hasRenderablePayload() ||
          !matchedLayer->bitmapEntityFromWorldTransform().has_value()) {
        eligible = false;
        break;
      }

      const auto& abs =
          components::LayoutSystem().getAbsoluteTransformComponent(EntityHandle(registry, e));
      const Transform2d canvasFromDocument =
          components::LayoutSystem().getCanvasFromDocumentTransform(registry);
      const Transform2d newWorldFromEntity =
          abs.worldFromEntity * (abs.worldIsCanvas ? canvasFromDocument : Transform2d());
      // `bitmapEntityFromEntity`: takes a point in the entity's CURRENT
      // frame and returns it in the bitmap's stamp-time entity frame.
      // The compositor uses this as `canvasFromBitmap` on the layer's
      // bitmap-reuse fast path: rendering the cached bitmap (which was
      // rasterized in stamp-frame coords) through this transform places
      // its pixels at the entity's current world position.
      //
      // Under donner's post-multiply convention (`A * B` applied to v
      // means "first B, then A"), `bitmapEntityFromWorld_at_stamp *
      // newWorldFromEntity_now` walks v from current entity → world →
      // stamp entity. The reverse order (`newWFE * oldEFW`) reads as a
      // direct entity-local mapping and collapses to the right answer
      // ONLY when the stamped transform's linear part is identity. For a
      // previously-resized element the reverse misreports the canvas
      // delta as `oldScale.inverse() * delta_doc`, so the cached bitmap
      // moves at `delta_doc / scale` pixels per drag step while the
      // overlay (driven by worldBounds) moves at `delta_doc`. See
      // `LayerComposeOffsetReflectsDocumentDeltaForResizedElement` for
      // the regression pin.
      const Transform2d bitmapEntityFromEntity =
          matchedLayer->bitmapEntityFromWorldTransform()->inverse() * newWorldFromEntity;

      const bool isSubtree = matchedLayer->firstEntity() != e || matchedLayer->lastEntity() != e;
      // Subtree layers require a pure-translation delta: only then can we
      // cheaply propagate the same offset to descendant RICs without
      // walking the layout system. Non-translation subtree mutations
      // (scale, rotate, transform-list change) stay on the slow path.
      // Single-entity layers can tolerate non-translation deltas via a
      // targeted re-rasterize later in `renderFrame`.
      if (isSubtree && !bitmapEntityFromEntity.isTranslation()) {
        eligible = false;
        break;
      }

      // Defensive guard (see `IsDomDescendantOf`): if another promoted
      // layer's entity is a descendant of our subtree root, translating
      // only this layer's `canvasFromBitmap` while leaving the child
      // layer's anchored to its rasterize-time transform would produce
      // ghosting. The invariants in `promoteEntity` and `MandatoryHint
      // Detector` should already prevent this, but bail to the slow path
      // rather than trust the invariant — the slow path will re-rasterize
      // correctly.
      if (isSubtree) {
        bool hasDescendantLayer = false;
        for (const auto& other : layers_) {
          if (&other == matchedLayer) continue;
          if (IsDomDescendantOf(registry, other.entity(), e)) {
            hasDescendantLayer = true;
            break;
          }
        }
        if (hasDescendantLayer) {
          eligible = false;
          break;
        }
      }

      resolutions.push_back(FastPathResolution{
          .entity = e,
          .layer = matchedLayer,
          .newWorldFromEntity = newWorldFromEntity,
          .bitmapEntityFromEntity = bitmapEntityFromEntity,
          .isSubtree = isSubtree,
      });
    }

    if (eligible) {
      std::vector<Entity> unhandledDirtyEntities;
      unhandledDirtyEntities.reserve(resolutions.size());
      for (const auto& res : resolutions) {
        auto& instance = registry.get<components::RenderingInstanceComponent>(res.entity);
        // Descendant RICs need only this frame's world-space delta. The
        // stamp-relative bitmap delta is cumulative and belongs only on the
        // cached bitmap's `canvasFromBitmap`.
        const Transform2d worldFromPreviousWorld =
            res.newWorldFromEntity * instance.worldFromEntityTransform.inverse();
        instance.worldFromEntityTransform = res.newWorldFromEntity;

        if (res.bitmapEntityFromEntity.isTranslation()) {
          // Bitmap-reuse fast path: the delta between the current DOM
          // transform and the bitmap's rasterize-time transform is a
          // pure translation, so reuse the bitmap by updating
          // `canvasFromBitmap_` instead of re-rasterizing. Every
          // mouse-move flips the entity's transform attribute, but the
          // compositor just writes a ~single-matrix compose offset
          // rather than going back to the renderer.
          res.layer->setCanvasFromBitmap(res.bitmapEntityFromEntity);
          if (res.isSubtree) {
            propagateFastPathTranslationToSubtree(registry, res.entity, worldFromPreviousWorld);
          }
        } else {
          // Single-entity layer with non-translation delta (scale,
          // rotate, etc.). Fall through to `consumeDirtyFlags` + the
          // per-layer re-rasterize — correctness over optimization
          // since the single entity has no descendants to worry about.
          res.layer->markDirty();
          unhandledDirtyEntities.push_back(res.entity);
        }
      }
      // Clear the dirty flags ourselves since we skipped prepare. Tell
      // the rest of renderFrame that the dirty state is fully resolved
      // — no need to re-run detectors, re-prepare, or re-resolve.
      registry.clear<components::DirtyFlagsComponent>();
      documentDirty = false;
      if (unhandledDirtyEntities.empty()) {
        fastPathTakenThisFrame = true;
      }
      dirtyEntitySnapshot = std::move(unhandledDirtyEntities);
    }
  }
  if (fastPathTakenThisFrame) {
    ++fastPathCounters_.fastPathFrames;
  } else if (!dirtyEntitySnapshot.empty() || needsFullRebuild) {
    ++fastPathCounters_.slowPathFramesWithDirty;
  }
  // On first renderFrame the `RenderingInstanceComponent` view is empty (it
  // gets built by `prepareDocumentForRendering` below), so the mandatory /
  // bucket detectors can't score anything useful. Defer them to the first
  // post-prepare frame by also rescanning whenever `hintsScanned_` is false.
  //
  // The same condition applies when `needsFullRebuild` is true:
  // `RenderingContext::invalidateRenderTree()` (called by
  // `SVGDocument::setCanvasSize` etc.) wipes every RIC. Running the
  // detector against the empty view here would mark every existing
  // mandatory hint as stale (the qualifying set is empty, see
  // `MandatoryHintDetector::reconcile`) and silently demote every
  // filter / mask / isolated-layer subtree until the user mutates the
  // document again. Skip the pre-prepare scan in that case and let the
  // post-prepare branch below pick the hints back up against the
  // freshly-rebuilt RIC view. Pinned by `MandatoryFilterLayerSurvives
  // CanvasResize`.
  const bool deferDetectorsToPostPrepare = needsFullRebuild;
  const bool needsHintRescan = !hintsScanned_ || documentDirty;
  if ((!documentPrepared_ || documentDirty) && documentPrepared_ && !deferDetectorsToPostPrepare) {
    // Document is already prepared and became dirty — rescan now.
    {
      ZoneScopedN("Compositor::mandatoryDetector.reconcile");
      mandatoryDetector_.reconcile(registry);
    }
    if (config_.complexityBucketing) {
      ZoneScopedN("Compositor::complexityBucketer.reconcile");
      complexityBucketer_.reconcile(registry);
    }
    hintsScanned_ = true;
  } else if (!documentPrepared_ && needsHintRescan) {
    // First frame, RICs don't exist yet. Skip the detectors — we'll run them
    // immediately after the first prepare below.
  }
  const ResolveOptions resolveOptions{
      .enableInteractionHints = config_.autoPromoteInteractions,
      .enableAnimationHints = config_.autoPromoteAnimations,
      .enableComplexityBucketHints = config_.complexityBucketing,
  };
  {
    ZoneScopedN("Compositor::resolver.resolve");
    resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
  }
  {
    ZoneScopedN("Compositor::reconcileLayers");
    reconcileLayers(registry);
  }
  // Only blow away every cached layer bitmap on a full tree rebuild
  // (`RenderTreeState::needsFullRebuild`) — that's when every entity handle
  // and RIC has been replaced and none of the old bitmaps are still keyed on
  // anything valid. Per-entity dirty flags (e.g., a single transform attribute
  // mutation from `SetTransformCommand` on drag release) intentionally do
  // NOT set `rootDirty_` here: `consumeDirtyFlags()` below inspects the
  // dirty set, marks just the affected layers, and escalates to `rootDirty_`
  // only when a dirty entity lives outside every promoted layer. Keeping
  // this fine-grained is what lets drag-release → drag-again on the same
  // entity reuse all the mandatory-filter bitmaps instead of paying the
  // full filter rasterization cost every time.
  if (needsFullRebuild) {
    rootDirty_ = true;
  }

  // If no promoted layers yet, take the simple full-render path. This
  // runs `prepareDocumentForRendering` implicitly via `driver.draw()`,
  // so by the time the detectors run afterwards they can observe RICs.
  //
  // Eager-warmup optimization: after the detectors pick up mandatory /
  // bucket layers, we rasterize them into their bitmap caches IN THE
  // SAME FRAME — so the user's first click-and-drag after load lands
  // on a warm compositor and doesn't freeze for several seconds while
  // every filter-group bitmap, every segment, and every bg/fg composite
  // rasterizes for the first time. The flat output the main renderer
  // just produced is the correct frame to show the user; the layer /
  // segment rasterization happens to offscreen targets and doesn't
  // affect this frame's pixels.
  if (layers_.empty()) {
    ZoneScopedN("Compositor::firstFrameEmptyLayersPath");
    RendererDriver driver(renderer());
    {
      ZoneScopedN("Compositor::driver.draw (first-frame)");
      driver.draw(document());
    }
    // `driver.draw` runs preparation for the first frame. Defensively clear
    // the rebuild flag so the eager-warmed compositor caches survive the next
    // `renderFrame`.
    if (registry.ctx().contains<components::RenderTreeState>()) {
      registry.ctx().get<components::RenderTreeState>().needsFullRebuild = false;
    }
    staticSegments_.clear();
    staticSegmentTextures_.clear();
    staticSegmentOffsets_.clear();
    staticSegmentLastRasterizeMs_.clear();
    staticSegmentsCanvas_ = Vector2i::Zero();
    staticSegmentsLayerCount_ = 0;
    splitStaticLayersEntity_ = entt::null;
    splitStaticLayersViewport_ = Vector2i::Zero();
    rootDirty_ = false;
    documentPrepared_ = true;

    // Populated RICs now exist. If the detectors haven't scanned yet, run
    // them once against the populated view and rebuild `layers_`. A newly
    // discovered mandatory filter/mask/isolated-layer promotes the affected
    // subtree immediately.
    if (!hintsScanned_) {
      ZoneScopedN("Compositor::firstFrameDetectorsAndWarmup");
      {
        ZoneScopedN("Compositor::mandatoryDetector.reconcile (first)");
        mandatoryDetector_.reconcile(registry);
      }
      if (config_.complexityBucketing) {
        ZoneScopedN("Compositor::complexityBucketer.reconcile (first)");
        complexityBucketer_.reconcile(registry);
      }
      hintsScanned_ = true;
      {
        ZoneScopedN("Compositor::resolver.resolve (first)");
        resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
      }
      {
        ZoneScopedN("Compositor::reconcileLayers (first)");
        reconcileLayers(registry);
      }
      refreshLayerMetadata();

      // Eager-warmup: for every layer the detectors just produced,
      // rasterize its bitmap immediately so the next render (likely the
      // user's first drag frame) doesn't have to pay the first-time
      // rasterize cost on the critical path. Also pre-rasterize static
      // segments against the new layer set. None of this writes to the
      // main renderer — all offscreen work.
      if (offscreenSupported_ ||
          (!offscreenSupportKnown_ && renderer().createOffscreenInstance() != nullptr)) {
        offscreenSupported_ = true;
        offscreenSupportKnown_ = true;
        {
          ZoneScopedN("Compositor::eagerWarmupRasterizeLayers");
          for (auto& layer : layers_) {
            if (!layer.hasRenderablePayload()) {
              rasterizeLayer(layer, viewport);
            }
          }
        }
        const Vector2i currentCanvasSize = document().canvasSize();
        // Seed segment state via the preserving resync path so
        // `staticSegmentBoundaries_` is consistent with the new layer
        // set. Nothing to preserve yet (segments are empty), so every
        // slot is marked dirty and rasterized.
        {
          ZoneScopedN("Compositor::resyncSegmentsToLayerSet (first)");
          resyncSegmentsToLayerSet(currentCanvasSize);
        }
        {
          ZoneScopedN("Compositor::rasterizeDirtyStaticSegments (first)");
          rasterizeDirtyStaticSegments(viewport);
        }
      } else {
        offscreenSupportKnown_ = true;
      }
    }
    return;
  }

  // Only prepare the document when it hasn't been prepared yet or when
  // dirty. `prepareDocumentForRendering` rebuilds the render instance tree,
  // which is expensive. During drag (composition transform changes only),
  // we skip preparation since the document content hasn't changed.
  //
  // `documentDirty` triggers prepare whenever ANY entity changed — we need
  // RICs rebuilt so the subsequent `consumeDirtyFlags` -> rasterize pass
  // sees the updated entity transforms. `rootDirty_` is reserved for the
  // more aggressive "invalidate all caches" path used on full-tree
  // rebuilds; individual entity dirty flags now flow through
  // `consumeDirtyFlags` for precise per-layer invalidation.
  if (!documentPrepared_ || documentDirty || rootDirty_) {
    ZoneScopedN("Compositor::prepareDocumentPath");
    ParseWarningSink warningSink;
    {
      ZoneScopedN("Compositor::prepareDocumentForRendering");
      RendererUtils::prepareDocumentForRendering(document(), /*verbose=*/false, warningSink);
    }
    documentPrepared_ = true;
    refreshLayerMetadata();

    if (dropNonRenderableInteractionHints(registry)) {
      resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
      reconcileLayers(registry);
      markAllSegmentsDirty();
    }

    // After preparation, keep the global rebuild signal consumed. RenderingContext clears this
    // after a successful render-tree instantiation; this write is a defensive no-op for callers
    // that still reach this path with the flag set.
    if (registry.ctx().contains<components::RenderTreeState>()) {
      registry.ctx().get<components::RenderTreeState>().needsFullRebuild = false;
    }

    // First prepare completed — run detectors now if they haven't yet,
    // OR re-run them when the pre-prepare branch deferred to here
    // because RICs had just been wiped by `invalidateRenderTree()`.
    if (!hintsScanned_ || deferDetectorsToPostPrepare) {
      ZoneScopedN("Compositor::detectorsAfterFirstPrepare");
      mandatoryDetector_.reconcile(registry);
      if (config_.complexityBucketing) {
        complexityBucketer_.reconcile(registry);
      }
      hintsScanned_ = true;
      resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
      reconcileLayers(registry);
    }
  }

  if (layers_.empty()) {
    ZoneScopedN("Compositor::emptyLayerSetAfterPrepare");
    RendererDriver driver(renderer());
    driver.draw(document());
    staticSegments_.clear();
    staticSegmentTextures_.clear();
    staticSegmentBoundaries_.clear();
    staticSegmentDirty_.clear();
    staticSegmentOffsets_.clear();
    staticSegmentLastRasterizeMs_.clear();
    staticSegmentsCanvas_ = Vector2i::Zero();
    staticSegmentsLayerCount_ = 0;
    splitStaticLayersEntity_ = entt::null;
    splitStaticLayersViewport_ = Vector2i::Zero();
    rootDirty_ = false;
    mainRendererHasCachedFrame_ = true;
    return;
  }

  // Update composition transforms for layers whose stamped transform
  // differs from their current RIC transform by a pure translation.
  // This covers the descendant-layer case: when a parent group drags,
  // its mandatory-promoted descendant layers (filter groups, clip-path
  // groups) get new RIC transforms via the cascade in
  // `prepareDocumentForRendering`, but aren't themselves in
  // `dirtyEntitySnapshot`, so the fast-path loop above misses them.
  // Without this pass, those layers' cached bitmaps compose at the
  // pre-drag stamp position while surrounding static segments re-
  // rasterize at the post-drag position, producing misalignment
  // crescents at the sub-layers' rendered edges (the
  // `#Clouds_with_gradients` splash artifact).
  // `worldFromEntityTransform` is stamped by `RenderingContext` as
  // `absoluteTransform × canvasFromDocument` (for worldIsCanvas
  // entities). With the canvas-from-canvas delta formula below
  // (`oldEFW * newWFE`), the result is already in canvas-pixel space
  // and feeds straight into the `setTransform` + `drawImage` compose
  // pipeline. `ScaledCanvasTranslationOnlyDragProducesCorrectPixels`
  // pins the 2× canvas (retina/zoomed) drag delta and
  // `LayerComposeOffsetReflectsDocumentDeltaForResizedElement` pins
  // the previously-resized-element drag delta.
  for (auto& layer : layers_) {
    if (!layer.hasRenderablePayload() || !layer.bitmapEntityFromWorldTransform().has_value()) {
      continue;
    }
    if (!registry.valid(layer.entity()) ||
        !registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
      continue;
    }
    const auto& instance = registry.get<components::RenderingInstanceComponent>(layer.entity());
    const Transform2d& bitmapStamp = *layer.bitmapEntityFromWorldTransform();
    // Direct canvas-to-canvas compose delta for the cached bitmap. The inverse
    // stamp walks from stamped canvas/entity space back to entity-local, then
    // the current RIC transform walks forward to the current canvas position.
    // `LayerComposeOffsetReflectsDocumentDeltaForResizedElement` pins the
    // non-identity-scale case.
    const Transform2d delta = bitmapStamp.inverse() * instance.worldFromEntityTransform;
    if (delta.isTranslation()) {
      layer.setCanvasFromBitmap(delta);
    }
    // Non-translation deltas (scale / rotate of an ancestor) require a
    // re-rasterize; `consumeDirtyFlags` / the rasterize loop handle
    // that via `layer.markDirty()`. We just silently leave those
    // layers alone here.
  }

  if (!offscreenSupportKnown_) {
    offscreenSupported_ = renderer().createOffscreenInstance() != nullptr;
    offscreenSupportKnown_ = true;
  }

  if (!offscreenSupported_) {
    RendererDriver driver(renderer());
    driver.draw(document());
    rootDirty_ = false;
    return;
  }

  // Check dirty flags on promoted entities. Uses the snapshot captured
  // before `prepareDocumentForRendering` cleared `DirtyFlagsComponent`.
  // This marks just the layers whose range contains a mutated entity;
  // everything else stays cached. Filter layers whose subtree didn't
  // change — the common case when the user drags an unrelated element —
  // keep their bitmaps through the mutation and the next drag starts
  // with the filter cache already warm.
  {
    ZoneScopedN("Compositor::consumeDirtyFlags");
    consumeDirtyFlags(dirtyEntitySnapshot);
  }

  // Re-rasterize dirty promoted layers. Dirty tracking subsumes what
  // `requiresConservativeFallback()` used to trigger here: the explicit
  // "conservative" invalidation above now flips `isDirty()` when the
  // document actually changed, so we don't need to force re-rasterization
  // of conservative-fallback layers on every frame. `rootDirty_` is
  // reserved for full-tree rebuilds; individual entity mutations flow
  // through `consumeDirtyFlags(dirtyEntitySnapshot)`. This is what keeps
  // drag-release → drag-again on the same entity from re-rasterizing
  // every mandatory filter layer.
  {
    ZoneScopedN("Compositor::rasterizeDirtyLayersLoop");
    for (auto& layer : layers_) {
      if (layer.isDirty() || !layer.hasRenderablePayload() || rootDirty_) {
        // §M4: bail between layer rasterizes. The remaining dirty
        // layers keep their `isDirty()` flag set, so the next
        // `renderFrame` finishes them. Returns directly out of
        // `renderFrame` rather than `break`-ing because subsequent
        // steps (`resyncSegmentsToLayerSet`, `composeLayers`) would
        // run against a partially-rasterized layer set and either
        // produce a torn composite snapshot or trip the dual-path
        // pixel-identity assertion.
        if (isCancelled()) {
          return;
        }
        rasterizeLayer(layer, viewport);
      }
    }
  }

  // Rasterize dirty static segments. `staticSegments_` holds one bitmap
  // per paint-order slot between promoted layers (plus the ends), and
  // `staticSegmentDirty_` tracks which slots need re-rasterization.
  // Structural changes (layer count, canvas size, `rootDirty_` from a
  // full tree rebuild) flip every slot dirty; per-entity mutations only
  // flip the containing slot dirty via `consumeDirtyFlags`.
  const Vector2i currentCanvasSize = document().canvasSize();
  // Resync `staticSegments_` to the current layer set, preserving
  // cached bitmaps whose boundary identity survived. When the user
  // clicks-to-drag, the ONLY segment that structurally changes is the
  // one the drag target's paint-order range falls inside of — it
  // splits in two. Every other segment (before-first-filter-group,
  // between-filter-groups-that-didn't-contain-the-target, after-last-
  // filter-group) keeps its cached bitmap. That's what gets click-to-
  // first-pixel under 100 ms on `donner_splash.svg` instead of
  // rebuilding every filter-group's inline neighbor from scratch.
  //
  // `rootDirty_` forces a full invalidation — used for structural
  // rebuilds where we can't trust any cached state (e.g., needsFull
  // Rebuild from a reparse that didn't take the structural remap).
  if (rootDirty_) {
    staticSegments_.clear();
    staticSegmentTextures_.clear();
    staticSegmentBoundaries_.clear();
    staticSegmentDirty_.clear();
    staticSegmentOffsets_.clear();
    staticSegmentLastRasterizeMs_.clear();
    staticSegmentsCanvas_ = Vector2i::Zero();
    staticSegmentsLayerCount_ = 0;
    splitStaticLayersEntity_ = entt::null;
    splitStaticLayersViewport_ = Vector2i::Zero();
    rootDirty_ = false;
  }
  bool anySegmentDirty = false;
  {
    ZoneScopedN("Compositor::resyncSegmentsToLayerSet");
    anySegmentDirty = resyncSegmentsToLayerSet(currentCanvasSize);
  }
  if (anySegmentDirty) {
    // Segment set changed; clear the drag-target tracking so the
    // next snapshot re-derives it.
    splitStaticLayersEntity_ = entt::null;
    splitStaticLayersViewport_ = Vector2i::Zero();
  }
  {
    ZoneScopedN("Compositor::cascadeTransformDirtyToDescendantSegments");
    cascadeTransformDirtyToDescendantSegments(transformDirtyEntities);
  }
  {
    ZoneScopedN("Compositor::rasterizeDirtyStaticSegments");
    rasterizeDirtyStaticSegments(viewport);
  }

  // In the single-drag-target split path, publish the editor-facing
  // bg/fg bitmaps. For `N=1` (just the drag layer promoted) they're
  // exactly the two cached segments — no composite needed. For `N>1`
  // (mandatory filter / bucket layers live alongside the drag target)
  // Design doc 0033 §M2C: the compositor no longer pre-flattens
  // segments + non-drag layers into bg/fg bitmaps. The editor reads
  // segments and layers directly via `snapshotTilesForUpload`, so the
  // only state we maintain here is *which* entity is the active drag
  // target. The tile snapshot uses `splitStaticLayersEntity_` to set
  // each tile's `isDragTarget` flag, which in turn drives the worker's
  // `dragTranslationDoc` extraction (`canvasFromBitmap` translation in
  // doc units).
  //
  // §M9 wrinkle: pending-demotion entries stay in `activeHints_` for
  // the hysteresis window, so a naive `activeHints_.size() == 1`
  // check would fall to `entt::null` whenever the user is mid-switch
  // between drag targets — dragTranslationDoc on the live tile would
  // stay at zero for the whole window (~30 frames; on a slow-path
  // worker at 4fps that's ~7s of "content stays put while overlay
  // tracks the cursor"). Count only entries NOT in
  // `pendingDemotions_` so the live drag target keeps its
  // `isDragTarget` flag.
  uint32_t liveHints = 0;
  Entity liveDragCandidate = entt::null;
  for (const auto& [hintEntity, hint] : activeHints_) {
    if (pendingDemotions_.contains(hintEntity)) {
      continue;
    }
    ++liveHints;
    liveDragCandidate = hintEntity;
  }
  if (liveHints == 1 && findLayer(liveDragCandidate) != nullptr) {
    splitStaticLayersEntity_ = liveDragCandidate;
    splitStaticLayersViewport_ = currentCanvasSize;
  } else {
    splitStaticLayersEntity_ = entt::null;
    splitStaticLayersViewport_ = Vector2i::Zero();
  }

  // Compose all layers onto the main target.
  {
    ZoneScopedN("Compositor::composeLayers");
    composeLayers(viewport);
  }

  // Dual-path pixel-identity assertion. When enabled, capture the composited
  // output, run a full-document reference render to the same renderer, then
  // compare pixel-by-pixel. Any drift fires a release-assert. Disabled by
  // default; CI compositor test targets flip this on via `CompositorConfig`.
  // Cost is 2x per frame, so don't enable for interactive editor use.
  if (config_.verifyPixelIdentity) {
    RendererBitmap compositedSnapshot = renderer().takeSnapshot();

    RendererDriver referenceDriver(renderer());
    referenceDriver.draw(document());
    RendererBitmap referenceSnapshot = renderer().takeSnapshot();

    if (compositedSnapshot.dimensions == referenceSnapshot.dimensions &&
        !compositedSnapshot.empty() && !referenceSnapshot.empty()) {
      size_t mismatch = 0;
      uint8_t maxDiff = 0;
      const size_t size =
          std::min(compositedSnapshot.pixels.size(), referenceSnapshot.pixels.size());
      for (size_t i = 0; i < size; ++i) {
        const uint8_t a = compositedSnapshot.pixels[i];
        const uint8_t b = referenceSnapshot.pixels[i];
        const uint8_t diff = a > b ? a - b : b - a;
        if (diff > 0) {
          ++mismatch;
          if (diff > maxDiff) {
            maxDiff = diff;
          }
        }
      }
      UTILS_RELEASE_ASSERT_MSG(
          mismatch == 0,
          "Compositor dual-path pixel-identity assertion failed: composited output "
          "differs from full-render reference. Check 0025 § Dual-path debug assertion.");
    }
  }
}

size_t CompositorController::layerCount() const {
  return layers_.size();
}

size_t CompositorController::totalBitmapMemory() const {
  size_t total = 0;
  for (const auto& segment : staticSegments_) {
    total += segment.pixels.size();
  }
  for (const auto& texture : staticSegmentTextures_) {
    if (texture == nullptr) {
      continue;
    }
    const Vector2i dims = texture->dimensions();
    total += static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y) * 4u;
  }
  for (const auto& layer : layers_) {
    total += layer.bitmap().pixels.size();
    if (layer.textureSnapshot() != nullptr) {
      const Vector2i dims = layer.textureSnapshot()->dimensions();
      total += static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y) * 4u;
    }
  }
  return total;
}

CompositorLayer* CompositorController::findLayer(Entity entity) {
  auto it = std::find_if(layers_.begin(), layers_.end(), [entity](const CompositorLayer& layer) {
    return layer.entity() == entity;
  });
  return it != layers_.end() ? &(*it) : nullptr;
}

const CompositorLayer* CompositorController::findLayer(Entity entity) const {
  auto it = std::find_if(layers_.begin(), layers_.end(), [entity](const CompositorLayer& layer) {
    return layer.entity() == entity;
  });
  return it != layers_.end() ? &(*it) : nullptr;
}

void CompositorController::rasterizeLayer(CompositorLayer& layer, const RenderViewport& viewport) {
  ZoneScopedN("Compositor::rasterizeLayer");
  const auto rasterizeStart = std::chrono::steady_clock::now();

  Registry& registry = document().registry();
  const LayerRasterGeometry geometry = ComputeLayerRasterGeometry(
      renderer(), registry, layer.firstEntity(), layer.lastEntity(), viewport);

  auto offscreen = renderer().createOffscreenInstance();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);
  RendererDriver driver(*offscreen);

  if (geometry.tight) {
    ZoneScopedN("Compositor::rasterizeLayer::drawEntityRangeTight");
    driver.drawEntityRange(registry, layer.firstEntity(), layer.lastEntity(), geometry.viewport,
                           geometry.surfaceFromCanvas);
  } else {
    driver.drawEntityRange(registry, layer.firstEntity(), layer.lastEntity(), geometry.viewport,
                           geometry.surfaceFromCanvas);
  }

  // Stamp the bitmap with the entity's current absolute transform so the
  // fast path in `renderFrame` can later tell whether a DOM transform
  // mutation is a pure translation (reuse bitmap via `canvasFromBitmap_`
  // delta) or a shape-changing transform (force re-rasterize). A missing
  // `RenderingInstanceComponent` means the entity was promoted before
  // the tree was prepared — treat it as transform identity, which makes
  // the subsequent fast-path delta equal to the new transform, which is
  // correct for the "bitmap was drawn at origin" case.
  Transform2d worldFromEntity;
  if (registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
    worldFromEntity = registry.get<components::RenderingInstanceComponent>(layer.entity())
                          .worldFromEntityTransform;
  }
  if (offscreen->requiresTextureSnapshotPresentation()) {
    std::shared_ptr<const RendererTextureSnapshot> texture = offscreen->takeTextureSnapshot();
    UTILS_RELEASE_ASSERT_MSG(
        texture != nullptr,
        "Geode compositor layer rasterization did not produce a GPU texture. Refusing CPU "
        "readback/upload fallback in Geode presentation mode.");
    layer.setTextureSnapshot(std::move(texture), worldFromEntity);
  } else {
    layer.setBitmap(offscreen->takeSnapshot(), worldFromEntity);
  }
  // `setBitmap`/`setTextureSnapshot` bump a per-object generation that resets
  // to 1 for every freshly-built layer. After a document replace reuses entity
  // ids, that "1" collides with the generation the editor's GL texture cache
  // already holds for the previous document's layer at the same id, so the new
  // pixels never upload. Stamp a process-monotonic generation (shared with
  // static segments) so each rasterization is globally unique.
  layer.setGeneration(nextTileGeneration_++);
  layer.setCanvasOffset(geometry.canvasOffset);
  const auto rasterizeEnd = std::chrono::steady_clock::now();
  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(rasterizeEnd - rasterizeStart).count();
  layer.setLastRasterizeMs(static_cast<double>(elapsedUs) / 1000.0);
  // Don't reset `canvasFromBitmap_` here — a caller may have set it
  // explicitly (tests, editor drag hand-off paths) and expect that
  // additional offset to apply on top of the freshly-rasterized bitmap.
  // The fast path in `renderFrame` is what updates `canvasFromBitmap_`
  // for DOM-driven deltas; rasterization itself just refreshes the
  // bitmap's content and the stamped `bitmapEntityFromWorldTransform`.
}

void CompositorController::rasterizeDirtyStaticSegments(const RenderViewport& viewport) {
  ZoneScopedN("Compositor::rasterizeDirtyStaticSegmentsImpl");
  Registry& registry = document().registry();
  const size_t layerCount = layers_.size();
  UTILS_RELEASE_ASSERT(staticSegments_.size() == layerCount + 1);
  UTILS_RELEASE_ASSERT(staticSegmentDirty_.size() == layerCount + 1);

  // Keep `staticSegmentOffsets_` parallel to `staticSegments_`. The
  // various cache-wipe sites (`resetAllLayers`, rootDirty path,
  // first-frame `layers_.empty()`) clear it; we grow it back here so
  // the per-segment writes below are always in bounds.
  if (staticSegmentOffsets_.size() != layerCount + 1) {
    staticSegmentOffsets_.resize(layerCount + 1, Vector2d::Zero());
  }
  if (staticSegmentTextures_.size() != layerCount + 1) {
    staticSegmentTextures_.resize(layerCount + 1);
  }
  if (staticSegmentLastRasterizeMs_.size() != layerCount + 1) {
    staticSegmentLastRasterizeMs_.resize(layerCount + 1, 0.0);
  }

  // Snapshot paint order once per frame. Each segment is a slice of this list,
  // rendered by entity range without mutating registry visibility.
  std::vector<Entity> paintOrder;
  {
    ZoneScopedN("Compositor::paintOrderSnapshot");
    const auto& storage = registry.storage<components::RenderingInstanceComponent>();
    paintOrder.reserve(storage.size());
    RenderingInstanceView view(registry);
    while (!view.done()) {
      paintOrder.push_back(view.currentEntity());
      view.advance();
    }
  }

  // Find each promoted layer's [first, last] indices in paint order.
  // Linear scan is fine — layerCount ≤ kMaxCompositorLayers (32) and
  // paintOrder is typically ~100 elements. Layers may have their root
  // entity at `firstEntity` and a subtree extending to `lastEntity`
  // elsewhere in the paint order — `computeEntityRange` stashed the
  // subtree's tail on the layer at `reconcileLayers` time.
  struct LayerPaintRange {
    size_t firstIdx = 0;
    size_t lastIdx = 0;
  };
  std::vector<LayerPaintRange> layerRanges(layerCount);
  for (size_t i = 0; i < layerCount; ++i) {
    const Entity first = layers_[i].firstEntity();
    const Entity last = layers_[i].lastEntity();
    size_t firstIdx = paintOrder.size();
    size_t lastIdx = paintOrder.size();
    for (size_t j = 0; j < paintOrder.size(); ++j) {
      if (paintOrder[j] == first) firstIdx = j;
      if (paintOrder[j] == last) {
        lastIdx = j;
        break;
      }
    }
    layerRanges[i] = {firstIdx, lastIdx};
  }

  for (size_t i = 0; i <= layerCount; ++i) {
    if (!staticSegmentDirty_[i]) {
      continue;
    }
    // §M4: bail between segment rasterizes. Leaves `staticSegmentDirty_`
    // intact for the slots we haven't reached yet so the next
    // `renderFrame` resumes the work.
    if (isCancelled()) {
      return;
    }
    ZoneScopedN("Compositor::rasterizeSegment");
    const auto segmentRasterizeStart = std::chrono::steady_clock::now();

    // Segment `i` spans paint order strictly between layer i-1 and
    // layer i (exclusive on both ends). Edge cases: segment 0 starts
    // at the document's first paint-ordered entity; segment N ends at
    // the last. If the computed range is empty (two layers back-to-
    // back in paint order), keep only an internal placeholder.
    const size_t startIdx = (i == 0) ? 0u : (layerRanges[i - 1].lastIdx + 1u);
    const size_t endIdx = (i == layerCount)
                              ? (paintOrder.empty() ? 0u : paintOrder.size() - 1u)
                              : (layerRanges[i].firstIdx == 0u ? 0u : layerRanges[i].firstIdx - 1u);

    const bool segmentIsEmpty =
        paintOrder.empty() || startIdx > endIdx || startIdx >= paintOrder.size();

    if (segmentIsEmpty) {
      // No entities in this segment's paint-order range. Use a 1×1
      // transparent placeholder so `.empty()` still means "not yet
      // rasterized"; public tile snapshots prune the placeholder.
      ZoneScopedN("Compositor::segment::emptyBitmap");
      RendererBitmap placeholder;
      placeholder.dimensions = Vector2i(1, 1);
      placeholder.rowBytes = 4u;
      placeholder.pixels.assign(4u, 0u);
      placeholder.alphaType = AlphaType::Premultiplied;
      staticSegments_[i] = std::move(placeholder);
      staticSegmentTextures_[i].reset();
      staticSegmentOffsets_[i] = Vector2d::Zero();
    } else {
      // Try the tight-bound path before allocating pixels. When the entity
      // range has precise canvas bounds, the offscreen is cropped and draws
      // are shifted by the crop origin.
      //
      // `computeEntityRangeBounds` returns `nullopt` for entity
      // ranges it can't precisely bound (text, markers, masks,
      // patterns, sub-documents — see its own contract in
      // `RendererDriver.h`). Callers treat `nullopt` as "fall back
      // to full-canvas"; never as "empty segment". See design doc
      // 0027-tight_bounded_segments.md for which cases are pending
      // precise handling.
      std::optional<Box2d> tightBoundsCanvas;
      if (config_.tightBoundedSegments) {
        ZoneScopedN("Compositor::segment::computeBounds");
        RendererDriver boundsDriver(renderer());
        tightBoundsCanvas = boundsDriver.computeEntityRangeBounds(
            registry, paintOrder[startIdx], paintOrder[endIdx], viewport, Transform2d());
      }
      // The bounds-compute overhead per segment is ~O(entities) with
      // no pixel work — negligible vs any render. But tight-bound
      // also adds the crop-into-smaller-bitmap overhead at compose
      // time; if the tight rect covers most of the canvas anyway,
      // the allocation savings don't justify it. Fall back to
      // full-canvas above the coverage threshold.
      constexpr double kTightBoundsCoverageThreshold = 0.75;
      const double canvasArea = viewport.size.x * viewport.size.y;
      bool useTight = false;
      Box2d tightBoundsSnapped;
      if (tightBoundsCanvas.has_value() && canvasArea > 0.0) {
        // Snap to integer pixels + 1px padding so AA edges aren't
        // clipped by the crop box — `computeEntityRangeBounds`
        // returns mathematical bounds, not pixel-aligned bounds.
        constexpr double kEdgePaddingPx = 1.0;
        const Vector2d padding(kEdgePaddingPx, kEdgePaddingPx);
        Box2d padded(tightBoundsCanvas->topLeft - padding,
                     tightBoundsCanvas->bottomRight + padding);
        const Vector2d snapTL(std::floor(std::max(0.0, padded.topLeft.x)),
                              std::floor(std::max(0.0, padded.topLeft.y)));
        const Vector2d snapBR(std::ceil(std::min(viewport.size.x, padded.bottomRight.x)),
                              std::ceil(std::min(viewport.size.y, padded.bottomRight.y)));
        if (snapBR.x > snapTL.x && snapBR.y > snapTL.y) {
          tightBoundsSnapped = Box2d(snapTL, snapBR);
          const double tightArea = tightBoundsSnapped.width() * tightBoundsSnapped.height();
          if (tightArea < canvasArea * kTightBoundsCoverageThreshold) {
            useTight = true;
          }
        }
      }

      std::unique_ptr<RendererInterface> offscreen;
      {
        ZoneScopedN("Compositor::segment::createOffscreen");
        offscreen = renderer().createOffscreenInstance();
      }
      UTILS_RELEASE_ASSERT(offscreen != nullptr);

      if (useTight) {
        ZoneScopedN("Compositor::segment::drawEntityRangeTight");
        RenderViewport tightViewport;
        tightViewport.size = tightBoundsSnapped.size();
        tightViewport.devicePixelRatio = viewport.devicePixelRatio;
        RendererDriver driver(*offscreen);
        driver.drawEntityRange(registry, paintOrder[startIdx], paintOrder[endIdx], tightViewport,
                               Transform2d::Translate(-tightBoundsSnapped.topLeft));
        if (offscreen->requiresTextureSnapshotPresentation()) {
          std::shared_ptr<const RendererTextureSnapshot> texture = offscreen->takeTextureSnapshot();
          UTILS_RELEASE_ASSERT_MSG(
              texture != nullptr,
              "Geode compositor segment rasterization did not produce a GPU texture. Refusing CPU "
              "readback/upload fallback in Geode presentation mode.");
          staticSegments_[i] = RendererBitmap{};
          staticSegmentTextures_[i] = std::move(texture);
        } else {
          staticSegments_[i] = offscreen->takeSnapshot();
          staticSegmentTextures_[i].reset();
        }
        staticSegmentOffsets_[i] = tightBoundsSnapped.topLeft;
      } else {
        ZoneScopedN("Compositor::segment::drawEntityRange");
        RendererDriver driver(*offscreen);
        driver.drawEntityRange(registry, paintOrder[startIdx], paintOrder[endIdx], viewport,
                               Transform2d());
        if (offscreen->requiresTextureSnapshotPresentation()) {
          std::shared_ptr<const RendererTextureSnapshot> texture = offscreen->takeTextureSnapshot();
          UTILS_RELEASE_ASSERT_MSG(
              texture != nullptr,
              "Geode compositor segment rasterization did not produce a GPU texture. Refusing CPU "
              "readback/upload fallback in Geode presentation mode.");
          staticSegments_[i] = RendererBitmap{};
          staticSegmentTextures_[i] = std::move(texture);
        } else {
          staticSegments_[i] = offscreen->takeSnapshot();
          staticSegmentTextures_[i].reset();
        }
        staticSegmentOffsets_[i] = Vector2d::Zero();
      }
    }
    staticSegmentDirty_[i] = false;
    // Bump the generation so the editor's GL texture cache sees this
    // slot's pixel content changed and re-uploads on next frame.
    if (i < staticSegmentGeneration_.size()) {
      staticSegmentGeneration_[i] = nextTileGeneration_++;
    }
    const auto segmentRasterizeEnd = std::chrono::steady_clock::now();
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               segmentRasterizeEnd - segmentRasterizeStart)
                               .count();
    if (i < staticSegmentLastRasterizeMs_.size()) {
      staticSegmentLastRasterizeMs_[i] = static_cast<double>(elapsedUs) / 1000.0;
    }
  }
}

bool CompositorController::resyncSegmentsToLayerSet(const Vector2i& currentCanvasSize) {
  const size_t newCount = layers_.size() + 1;

  // Compute the NEW boundary identity for each slot. Slot i sits between
  // layers_[i-1] and layers_[i]; the edge cases at i==0 and i==N use
  // entt::null as the "beyond the document" sentinel.
  std::vector<std::pair<Entity, Entity>> newBoundaries(newCount);
  for (size_t i = 0; i < newCount; ++i) {
    const Entity left = (i == 0) ? entt::null : layers_[i - 1].entity();
    const Entity right = (i == layers_.size()) ? entt::null : layers_[i].entity();
    newBoundaries[i] = {left, right};
  }

  // Canvas resized — bitmap caches are sized to the old canvas and can't
  // be reused at a different resolution (would be scaled or leave
  // transparent gaps). Full invalidation.
  const bool canvasChanged = staticSegmentsCanvas_ != currentCanvasSize;

  std::vector<RendererBitmap> newSegments(newCount);
  std::vector<std::shared_ptr<const RendererTextureSnapshot>> newTextures(newCount);
  std::vector<bool> newDirty(newCount, true);
  std::vector<uint64_t> newGeneration(newCount, 0);
  std::vector<Vector2d> newOffsets(newCount, Vector2d::Zero());
  std::vector<double> newLastRasterizeMs(newCount, 0.0);

  if (!canvasChanged && !staticSegments_.empty()) {
    // Build a lookup from old boundary identity → (old slot index).
    // Small N (kMaxCompositorLayers = 32), so a linear scan beats a hash.
    for (size_t i = 0; i < newCount; ++i) {
      const auto& [left, right] = newBoundaries[i];
      for (size_t j = 0; j < staticSegmentBoundaries_.size(); ++j) {
        if (staticSegmentBoundaries_[j].first == left &&
            staticSegmentBoundaries_[j].second == right) {
          newSegments[i] = std::move(staticSegments_[j]);
          if (j < staticSegmentTextures_.size()) {
            newTextures[i] = std::move(staticSegmentTextures_[j]);
          }
          const bool wasDirty = j < staticSegmentDirty_.size() ? staticSegmentDirty_[j] : false;
          newDirty[i] = wasDirty || (newSegments[i].empty() && newTextures[i] == nullptr);
          if (j < staticSegmentGeneration_.size()) {
            // Preserve the old slot's generation so the editor's GL
            // texture cache reuses the existing binding — no upload.
            newGeneration[i] = staticSegmentGeneration_[j];
          }
          if (j < staticSegmentOffsets_.size()) {
            // Preserve the tight-bound offset alongside the bitmap —
            // the two together describe where the segment lives on
            // the canvas. A preserved bitmap with its offset dropped
            // would blit at (0,0) and wreck the compose.
            newOffsets[i] = staticSegmentOffsets_[j];
          }
          if (j < staticSegmentLastRasterizeMs_.size()) {
            newLastRasterizeMs[i] = staticSegmentLastRasterizeMs_[j];
          }
          break;
        }
      }
    }
  }

  // Slots with no preserved generation are net-new (the drag target
  // split an existing segment, or this is the first rasterization
  // ever). Mint them a fresh generation; the editor's GL texture
  // cache will see a new tileId→generation pair and upload.
  for (size_t i = 0; i < newCount; ++i) {
    if (newGeneration[i] == 0) {
      newGeneration[i] = nextTileGeneration_++;
    }
  }

  staticSegments_ = std::move(newSegments);
  staticSegmentTextures_ = std::move(newTextures);
  staticSegmentDirty_ = std::move(newDirty);
  staticSegmentBoundaries_ = std::move(newBoundaries);
  staticSegmentGeneration_ = std::move(newGeneration);
  staticSegmentOffsets_ = std::move(newOffsets);
  staticSegmentLastRasterizeMs_ = std::move(newLastRasterizeMs);
  staticSegmentsCanvas_ = currentCanvasSize;
  staticSegmentsLayerCount_ = layers_.size();

  // bg/fg cache is ONLY reusable if every constituent bitmap survived.
  // Any preserved-segment map-miss means at least one rasterize is
  // pending, so bg/fg needs recompositing regardless. Caller drops the
  // cache when we return true.
  return canvasChanged || std::any_of(staticSegmentDirty_.begin(), staticSegmentDirty_.end(),
                                      [](bool dirty) { return dirty; });
}

namespace {

// Encode a segment's boundary pair into a stable 64-bit tileId. Two
// 32-bit entity ids pack into the low 63 bits; the high bit (bit 63)
// stays 0 to distinguish from layer tiles.
constexpr uint64_t kLayerTileBit = 1ull << 63;

uint64_t SegmentTileId(Entity left, Entity right) {
  // entt::null for "beyond the document start/end" is a very large
  // sentinel value; fold to 0 so (null, X) differs from (Y, null) by
  // which half is the sentinel, and so boundary pairs that match
  // between old and new layer sets produce the same packed id.
  const auto normalize = [](Entity e) -> uint32_t {
    return (e == entt::null) ? 0u : static_cast<uint32_t>(entt::to_integral(e));
  };
  const uint64_t l = normalize(left);
  const uint64_t r = normalize(right);
  return (l << 32) | r;  // bit 63 stays 0 — doesn't collide with layer ids.
}

}  // namespace

std::vector<CompositorTile> CompositorController::snapshotTilesForUpload(
    CompositorTileBitmapPayload payload) const {
  // Interleaved tile list: segment[0], layer[0], segment[1], layer[1],
  // ..., layer[N-1], segment[N]. Matches the paint-order composition
  // in `composeLayers` so the editor can blit tiles by iterating the
  // vector in order.
  //
  // Segment tile ids encode the boundary pair (left-layer entity, right
  // -layer entity) so they're STABLE ACROSS INDEX SHIFTS — when a drag
  // target splits segment K into two, the segments at positions K-1
  // and K+1 in the new set (which are preserved content from old K-1
  // and K+1) keep the same tileId. The editor's GL texture cache keyed
  // on `tileId` recognizes the preservation and skips re-upload even
  // though the index position of the texture in the new interleaved
  // list shifted by one.
  //
  // Layer tile ids are `(1ull << 63) | entityId` — bit 63 is the
  // segment-vs-layer discriminator.
  std::vector<CompositorTile> tiles;
  tiles.reserve(layers_.size() + staticSegments_.size());
  uint32_t paintIdx = 0;
  const auto segmentOffsetAt = [this](size_t idx) {
    return idx < staticSegmentOffsets_.size() ? staticSegmentOffsets_[idx] : Vector2d::Zero();
  };
  const auto segmentTextureAt =
      [this](size_t idx) -> std::shared_ptr<const RendererTextureSnapshot> {
    return idx < staticSegmentTextures_.size() ? staticSegmentTextures_[idx] : nullptr;
  };
  const auto includePayload = [payload](bool hasPayload, bool isDragTarget) {
    if (!hasPayload) return false;
    switch (payload) {
      case CompositorTileBitmapPayload::All: return true;
      case CompositorTileBitmapPayload::DragTargetOnly: return isDragTarget;
      case CompositorTileBitmapPayload::MetadataOnly: return false;
    }
    return false;
  };
  for (size_t i = 0; i < layers_.size(); ++i) {
    // Segment slot before layer[i]. Tile id = (left-neighbor entity,
    // right-neighbor entity).
    const auto& boundary = (i < staticSegmentBoundaries_.size())
                               ? staticSegmentBoundaries_[i]
                               : std::pair<Entity, Entity>{entt::null, entt::null};
    const uint64_t segGen = (i < staticSegmentGeneration_.size()) ? staticSegmentGeneration_[i] : 0;
    const RendererBitmap* segBitmap =
        (i < staticSegments_.size() && HasPublicTileBitmap(staticSegments_[i]))
            ? &staticSegments_[i]
            : nullptr;
    const std::shared_ptr<const RendererTextureSnapshot> segTexture = segmentTextureAt(i);
    if (segBitmap != nullptr || segTexture != nullptr) {
      const bool includeSegPayload = includePayload(/*hasPayload=*/true, /*isDragTarget=*/false);
      const Vector2i bitmapDims =
          segBitmap != nullptr ? segBitmap->dimensions : segTexture->dimensions();
      tiles.push_back(CompositorTile{
          .tileId = SegmentTileId(boundary.first, boundary.second),
          .generation = segGen,
          .paintOrderIndex = paintIdx++,
          .bitmap = includeSegPayload && segBitmap != nullptr ? *segBitmap : RendererBitmap{},
          .textureSnapshot = includeSegPayload ? segTexture : nullptr,
          .bitmapDims = bitmapDims,
          .layerEntity = entt::null,
          .canvasOffsetPx = segmentOffsetAt(i),
          .canvasFromBitmap = Transform2d(),
          .isDragTarget = false,
      });
    }
    // Layer tile.
    const auto& layer = layers_[i];
    const RendererBitmap* layerBitmap = layer.hasValidBitmap() ? &layer.bitmap() : nullptr;
    const std::shared_ptr<const RendererTextureSnapshot> layerTexture = layer.textureSnapshot();
    const bool isDragTarget = layer.entity() == splitStaticLayersEntity_;
    const bool includeLayerPayload =
        includePayload(layerBitmap != nullptr || layerTexture != nullptr, isDragTarget);
    tiles.push_back(CompositorTile{
        .tileId = kLayerTileBit | static_cast<uint64_t>(entt::to_integral(layer.entity())),
        .generation = layer.generation(),
        .paintOrderIndex = paintIdx++,
        .bitmap = includeLayerPayload && layerBitmap != nullptr ? *layerBitmap : RendererBitmap{},
        .textureSnapshot = includeLayerPayload ? layerTexture : nullptr,
        .bitmapDims = layerBitmap != nullptr ? layerBitmap->dimensions
                                             : (layerTexture != nullptr ? layerTexture->dimensions()
                                                                        : Vector2i::Zero()),
        .layerEntity = layer.entity(),
        .canvasOffsetPx = layer.canvasOffset(),
        .canvasFromBitmap = layer.canvasFromBitmap(),
        .isDragTarget = isDragTarget,
    });
  }
  // Trailing segment after the last layer.
  const size_t tailIdx = layers_.size();
  const auto& tailBoundary = (tailIdx < staticSegmentBoundaries_.size())
                                 ? staticSegmentBoundaries_[tailIdx]
                                 : std::pair<Entity, Entity>{entt::null, entt::null};
  const uint64_t tailGen =
      (tailIdx < staticSegmentGeneration_.size()) ? staticSegmentGeneration_[tailIdx] : 0;
  const RendererBitmap* tailBitmap =
      (tailIdx < staticSegments_.size() && HasPublicTileBitmap(staticSegments_[tailIdx]))
          ? &staticSegments_[tailIdx]
          : nullptr;
  const std::shared_ptr<const RendererTextureSnapshot> tailTexture = segmentTextureAt(tailIdx);
  if (tailBitmap != nullptr || tailTexture != nullptr) {
    const bool includeTailPayload = includePayload(/*hasPayload=*/true, /*isDragTarget=*/false);
    const Vector2i bitmapDims =
        tailBitmap != nullptr ? tailBitmap->dimensions : tailTexture->dimensions();
    tiles.push_back(CompositorTile{
        .tileId = SegmentTileId(tailBoundary.first, tailBoundary.second),
        .generation = tailGen,
        .paintOrderIndex = paintIdx,
        .bitmap = includeTailPayload && tailBitmap != nullptr ? *tailBitmap : RendererBitmap{},
        .textureSnapshot = includeTailPayload ? tailTexture : nullptr,
        .bitmapDims = bitmapDims,
        .layerEntity = entt::null,
        .canvasOffsetPx = segmentOffsetAt(tailIdx),
        .canvasFromBitmap = Transform2d(),
        .isDragTarget = false,
    });
  }
  return tiles;
}

size_t CompositorController::findSegmentForEntity(Entity entity) const {
  Registry& registry = document().registry();
  if (!registry.valid(entity) || !registry.all_of<components::RenderingInstanceComponent>(entity)) {
    return layers_.size();
  }
  const int entityDrawOrder =
      registry.get<components::RenderingInstanceComponent>(entity).drawOrder;
  for (size_t i = 0; i < layers_.size(); ++i) {
    const Entity layerFirst = layers_[i].firstEntity();
    if (!registry.valid(layerFirst) ||
        !registry.all_of<components::RenderingInstanceComponent>(layerFirst)) {
      continue;
    }
    const int layerFirstDrawOrder =
        registry.get<components::RenderingInstanceComponent>(layerFirst).drawOrder;
    if (entityDrawOrder < layerFirstDrawOrder) {
      return i;
    }
  }
  return layers_.size();
}

void CompositorController::markAllSegmentsDirty() {
  staticSegmentDirty_.assign(layers_.size() + 1, true);
}

void CompositorController::composeLayers(const RenderViewport& viewport) {
  ZoneScopedN("Compositor::composeLayersImpl");

  // Split path: bg/fg already composite segments + non-drag promoted
  // layers internally (see `recomposeSplitBitmaps` / `N=1` fast path), so
  // the main-renderer compose collapses to 3 `drawImage` calls — one for
  // bg, one for the drag layer at its current compose offset, and one
  // for fg. With a 5-layer splash that's 3 `drawImage` + 3 `Unpremultiply
  // Pixels` passes per frame instead of the 2N+1 = 11 the naive
  // segment/layer interleave would pay.
  //
  // Non-split path (no drag, or multiple drag targets): walk the full
  // interleave. Each segment holds non-promoted content only, each
  // layer is independent, so paint order is preserved.
  //
  // When the editor is composing the drag overlay itself via GL, the
  // main-renderer compose output is only needed for the next full-canvas
  // composited tile. Re-running 3+ full-canvas drawImage calls into a
  // snapshot nobody will read during active drag burned ~110 ms/frame at
  // 892×512 on Skia, so we skip the compose when
  // `skipMainComposeDuringSplit_` is on and the split cache is populated.
  //
  // The `skipMainCompose` path also skips `beginFrame`/`endFrame`
  // entirely: `beginFrame` recreates the renderer's pixmap as a fully
  // transparent buffer, so calling it and then NOT drawing anything would
  // leave a transparent CPU snapshot. Skipping both begin and end preserves
  // the last full-canvas render as the source for any later full-canvas
  // composited tile.
  // The post-drag settle render runs with `skipMainComposeDuringSplit_`
  // effectively off (via the settling-render path in AsyncRenderer), so
  // the full-canvas snapshot is refreshed before it can seed a tile.
  //
  // The skip is ALSO gated on "an ActiveDrag is in flight": selection-
  // only prewarm renders (e.g., mouse-hovering a selected element before
  // any drag) must still produce a fresh full-canvas snapshot. Check
  // `activeHints_` for a kind-ActiveDrag entry, not just "split layers
  // present" — a Selection-only promote produces split layers too.
  const bool hasActiveDrag = [this]() {
    for (const auto& [entity, hint] : activeHints_) {
      if (hint.interactionKind() == InteractionHint::ActiveDrag) {
        return true;
      }
    }
    return false;
  }();
  // First-frame guard: if the main renderer has no cached frame yet,
  // we MUST run the full compose so callers that read `takeSnapshot`
  // (full-canvas tile creation, unit tests) get valid pixels. After the
  // first full compose lands, subsequent drag frames can safely skip because
  // `frame_` retains the prior pixmap.
  const bool skipMainCompose = skipMainComposeDuringSplit_ && hasActiveDrag &&
                               hasSplitStaticLayers() && mainRendererHasCachedFrame_;
  if (skipMainCompose) {
    return;
  }

  renderer().beginFrame(viewport);

  const auto drawPayload = [this](const RendererBitmap& bitmap,
                                  const std::shared_ptr<const RendererTextureSnapshot>& texture,
                                  const Transform2d& canvasFromPayload) {
    const Vector2i payloadDims =
        HasPublicTileBitmap(bitmap)
            ? bitmap.dimensions
            : (texture != nullptr ? texture->dimensions() : Vector2i::Zero());
    if (payloadDims.x <= 0 || payloadDims.y <= 0) {
      return;
    }
    ImageParams params;
    params.targetRect = Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(payloadDims.x),
                                                         static_cast<double>(payloadDims.y)));
    renderer().setTransform(canvasFromPayload);
    if (texture != nullptr) {
      const bool drewTexture = renderer().drawTextureSnapshot(*texture, params.targetRect);
      UTILS_RELEASE_ASSERT_MSG(
          drewTexture || !renderer().requiresTextureSnapshotPresentation(),
          "Geode compositor compose could not draw a GPU texture payload. Refusing CPU "
          "bitmap fallback in Geode presentation mode.");
      if (drewTexture) {
        return;
      }
    }
    UTILS_RELEASE_ASSERT_MSG(!renderer().requiresTextureSnapshotPresentation(),
                             "Geode compositor compose received a CPU bitmap payload. Refusing CPU "
                             "bitmap fallback in Geode presentation mode.");
    if (renderer().requiresTextureSnapshotPresentation()) {
      return;
    }
    if (HasPublicTileBitmap(bitmap)) {
      ImageResource image = BuildImageResource(bitmap);
      renderer().drawImage(image, params);
    }
  };

  const auto drawLayer = [&](const CompositorLayer& layer) {
    if (!layer.hasRenderablePayload()) {
      return;
    }
    // Compose = Translate(canvasOffset) * canvasFromBitmap. `canvasOffset`
    // places intrinsic-sized layer rasters back on the canvas; `canvasFromBitmap`
    // carries post-rasterize DOM drift. Snap pure translations to integer pixels.
    const Vector2d offset = layer.canvasOffset();
    Transform2d canvasFromBitmap = layer.canvasFromBitmap();
    if (canvasFromBitmap.isTranslation()) {
      const Vector2d t = canvasFromBitmap.translation();
      canvasFromBitmap =
          Transform2d::Translate(std::round(t.x + offset.x), std::round(t.y + offset.y));
    } else {
      canvasFromBitmap = Transform2d::Translate(offset) * canvasFromBitmap;
    }
    drawPayload(layer.bitmap(), layer.textureSnapshot(), canvasFromBitmap);
  };

  // Design doc 0033 §M2C: compose static segments and promoted layers in
  // interleaved paint order. Active drag frames may skip this main-renderer
  // compose via the `skipMainCompose` gate above.
  if (!staticSegments_.empty()) {
    for (size_t i = 0; i < layers_.size(); ++i) {
      const Vector2d segmentOffset =
          i < staticSegmentOffsets_.size() ? staticSegmentOffsets_[i] : Vector2d::Zero();
      const std::shared_ptr<const RendererTextureSnapshot> segmentTexture =
          i < staticSegmentTextures_.size() ? staticSegmentTextures_[i] : nullptr;
      drawPayload(staticSegments_[i], segmentTexture, Transform2d::Translate(segmentOffset));
      drawLayer(layers_[i]);
    }
    const Vector2d lastOffset = staticSegmentOffsets_.size() == staticSegments_.size()
                                    ? staticSegmentOffsets_.back()
                                    : Vector2d::Zero();
    const std::shared_ptr<const RendererTextureSnapshot> lastTexture =
        staticSegmentTextures_.size() == staticSegments_.size() ? staticSegmentTextures_.back()
                                                                : nullptr;
    drawPayload(staticSegments_.back(), lastTexture, Transform2d::Translate(lastOffset));
  }

  renderer().endFrame();
  // Record that the main renderer's framebuffer now holds a full
  // compose — future drag frames can safely skip `composeLayers` and
  // `takeSnapshot` will still return a valid full-canvas snapshot.
  mainRendererHasCachedFrame_ = true;
}

void CompositorController::cascadeTransformDirtyToDescendantSegments(
    const std::vector<Entity>& transformDirtyEntities) {
  Registry& registry = document().registry();
  using TreeComponent = donner::components::TreeComponent;
  for (const Entity entity : transformDirtyEntities) {
    // Also dirty the segment containing `entity` itself — the baseline
    // `consumeDirtyFlags` path may have dirtied a layer instead, but if
    // we're here the entity had a transform flag and re-rasterizing its
    // own segment is needed when the entity is in a layer that doesn't
    // re-raster (composition-transform path).
    //
    // EXCEPTION: if the entity is a promoted layer ROOT (non-zero
    // `ComputedLayerAssignmentComponent::layerId` and the layer is
    // rooted at this entity), its whole subtree lives in the layer's
    // cached bitmap — NOT in any static segment. The layer's bitmap is
    // either reused via a compose-transform (fast path) or re-rasterized
    // via `rasterizeLayer` (slow path). Marking the root or descendants'
    // segments dirty just forces unnecessary static-segment rasterize +
    // bg/fg recomposition on every drag frame.
    const auto* selfAssignment = registry.try_get<ComputedLayerAssignmentComponent>(entity);
    const bool entityIsPromotedLayerRoot =
        selfAssignment != nullptr && selfAssignment->layerId != 0;
    if (entityIsPromotedLayerRoot) {
      continue;
    }
    if (registry.all_of<components::RenderingInstanceComponent>(entity)) {
      const size_t selfSegIdx = findSegmentForEntity(entity);
      if (selfSegIdx < staticSegmentDirty_.size()) {
        staticSegmentDirty_[selfSegIdx] = true;
      }
    }
    std::vector<Entity> stack;
    if (const auto* tree = registry.try_get<TreeComponent>(entity)) {
      for (Entity child = tree->firstChild(); child != entt::null;
           child = registry.get<TreeComponent>(child).nextSibling()) {
        stack.push_back(child);
      }
    }
    while (!stack.empty()) {
      const Entity descendant = stack.back();
      stack.pop_back();
      const auto* assignment = registry.try_get<ComputedLayerAssignmentComponent>(descendant);
      if (assignment != nullptr && assignment->layerId != 0) {
        continue;  // sub-layer: handled by its own canvasFromBitmap.
      }
      if (registry.all_of<components::RenderingInstanceComponent>(descendant)) {
        const size_t segIdx = findSegmentForEntity(descendant);
        if (segIdx < staticSegmentDirty_.size()) {
          staticSegmentDirty_[segIdx] = true;
        }
      }
      if (const auto* dtree = registry.try_get<TreeComponent>(descendant)) {
        for (Entity grandchild = dtree->firstChild(); grandchild != entt::null;
             grandchild = registry.get<TreeComponent>(grandchild).nextSibling()) {
          stack.push_back(grandchild);
        }
      }
    }
  }
}

void CompositorController::consumeDirtyFlags(const std::vector<Entity>& dirtyEntities) {
  Registry& registry = document().registry();

  // Keep `staticSegmentDirty_` sized correctly so we can flip per-segment
  // flags below. A fresh compositor (or one whose layer set just changed)
  // arrives here with a smaller/larger vector.
  if (staticSegmentDirty_.size() != layers_.size() + 1) {
    staticSegmentDirty_.assign(layers_.size() + 1, true);
  }

  for (const Entity entity : dirtyEntities) {
    bool matchedPromotedRange = false;
    for (auto& layer : layers_) {
      if (!registry.valid(layer.entity())) {
        layer.markDirty();
        continue;
      }

      if (layerContainsEntity(layer, entity)) {
        layer.markDirty();
        matchedPromotedRange = true;
      }
    }

    if (!matchedPromotedRange) {
      // Per-segment dirty: mark just the paint-order slot that contains
      // this entity. Every other segment (and every promoted layer)
      // stays cached, so a mutation on a non-promoted rect at the far
      // end of the document doesn't invalidate the whole static tree.
      const size_t segmentIdx = findSegmentForEntity(entity);
      if (segmentIdx < staticSegmentDirty_.size()) {
        staticSegmentDirty_[segmentIdx] = true;
      } else {
        rootDirty_ = true;
      }
    }
  }

  // Global render tree state.
  if (registry.ctx().contains<components::RenderTreeState>()) {
    auto& state = registry.ctx().get<components::RenderTreeState>();
    if (state.needsFullRebuild) {
      rootDirty_ = true;
      markAllSegmentsDirty();
      for (auto& layer : layers_) {
        layer.markDirty();
      }
    }
  }
}

void CompositorController::refreshLayerMetadata() {
  Registry& registry = document().registry();
  for (auto& layer : layers_) {
    if (!registry.valid(layer.entity())) {
      continue;
    }

    const auto [firstEntity, lastEntity] = computeEntityRange(registry, layer.entity());
    layer.setEntityRange(firstEntity, lastEntity);

    if (registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
      const auto& instance = registry.get<components::RenderingInstanceComponent>(layer.entity());
      layer.setFallbackReasons(detectFallbackReasons(instance));
    } else {
      layer.setFallbackReasons(FallbackReason::None);
    }
  }
}

void CompositorController::reconcileLayers(Registry& registry) {
  // Step 1: drop layers whose entity no longer has a non-zero assignment.
  const auto removeIt =
      std::remove_if(layers_.begin(), layers_.end(), [&registry](const CompositorLayer& layer) {
        if (!registry.valid(layer.entity())) {
          return true;
        }
        const auto* assignment = registry.try_get<ComputedLayerAssignmentComponent>(layer.entity());
        if (assignment == nullptr || assignment->layerId == 0) {
          return true;
        }
        return false;
      });
  if (removeIt != layers_.end()) {
    layers_.erase(removeIt, layers_.end());
  }

  // Step 2: add or refresh layers for entities whose assignment is non-zero.
  auto view = registry.view<ComputedLayerAssignmentComponent>();
  for (const Entity entity : view) {
    const auto& assignment = view.get<ComputedLayerAssignmentComponent>(entity);
    if (assignment.layerId == 0) {
      continue;
    }

    CompositorLayer* existing = findLayer(entity);
    if (existing != nullptr) {
      // Preserve bitmap / dirty / composition transform; update id if the
      // resolver reassigned it (e.g. a higher-weight neighbor demoted).
      if (existing->id() != assignment.layerId) {
        existing->setLayerId(assignment.layerId);
      }
      continue;
    }

    const auto [firstEntity, lastEntity] = computeEntityRange(registry, entity);
    layers_.emplace_back(assignment.layerId, entity, firstEntity, lastEntity);

    if (registry.all_of<components::RenderingInstanceComponent>(entity)) {
      const auto& instance = registry.get<components::RenderingInstanceComponent>(entity);
      layers_.back().setFallbackReasons(detectFallbackReasons(instance));
    }

    // A layer-set change is handled by `resyncSegmentsToLayerSet`, which
    // preserves cached bitmaps for segments whose boundary identity survived.
  }

  // Sort layers by the draw order of their root entity. `composeLayers()`
  // iterates `layers_` in order when blitting, so this is what guarantees a
  // multi-layer scene composes in SVG paint order (earlier draw-order below,
  // later above). Single-layer scenes are a no-op. Stable sort preserves
  // insertion order for entities with identical `drawOrder` (shouldn't happen
  // in well-instantiated documents, but we don't want to introduce
  // nondeterminism if it does).
  std::stable_sort(layers_.begin(), layers_.end(),
                   [&registry](const CompositorLayer& a, const CompositorLayer& b) {
                     const auto* aInstance =
                         registry.try_get<components::RenderingInstanceComponent>(a.entity());
                     const auto* bInstance =
                         registry.try_get<components::RenderingInstanceComponent>(b.entity());
                     const int aDrawOrder = aInstance != nullptr ? aInstance->drawOrder : 0;
                     const int bDrawOrder = bInstance != nullptr ? bInstance->drawOrder : 0;
                     return aDrawOrder < bDrawOrder;
                   });
}

bool CompositorController::layerContainsEntity(const CompositorLayer& layer, Entity entity) const {
  Registry& registry = document().registry();
  if (!registry.valid(layer.firstEntity()) || !registry.valid(layer.lastEntity()) ||
      !registry.valid(entity)) {
    return false;
  }

  RenderingInstanceView view(registry);
  while (!view.done() && view.currentEntity() != layer.firstEntity()) {
    view.advance();
  }

  while (!view.done()) {
    const Entity current = view.currentEntity();
    if (current == entity) {
      return true;
    }
    if (current == layer.lastEntity()) {
      return false;
    }
    view.advance();
  }

  return false;
}

std::pair<Entity, Entity> CompositorController::computeEntityRange(Registry& registry,
                                                                   Entity entity) {
  if (registry.all_of<components::RenderingInstanceComponent>(entity)) {
    const auto& instance = registry.get<components::RenderingInstanceComponent>(entity);
    if (instance.subtreeInfo.has_value()) {
      return {entity, instance.subtreeInfo->lastRenderedEntity};
    }
  }

  // No `SubtreeInfo` — the entity's RIC didn't establish an isolated
  // rendering scope (plain `<g>`, e.g.). Walk the DOM tree to find
  // the last RIC-bearing descendant so the promoted layer's
  // paint-order range covers every descendant that paints.
  //
  // Rationale: if the range stays at {entity, entity}, the layer's
  // bitmap rasterizes only the group root (usually a no-op draw) and
  // the children end up in an adjacent static segment. When the
  // group's transform mutates on drag, `consumeDirtyFlags` matches
  // the entity against its single-entity layer range and stops;
  // segments carrying descendants never get marked dirty, so cached
  // bitmaps keep the children at pre-drag positions — visible as
  // "children don't move when I drag their parent." See compositor
  // golden `TwoPhaseDragOfPlainGroupMovesChildren`.
  //
  // Widening here also disables the single-entity fast path
  // (eligibility requires `firstEntity == lastEntity == e`, see
  // `renderFrame`), forcing `prepareDocumentForRendering` to run on
  // the drag-start frame. That's the correct behavior: the full
  // RIC rebuild cascades transforms to descendants. Subsequent
  // same-entity drag frames (steady state) hit the bitmap-reuse fast
  // path via `canvasFromBitmap`, so interactivity is preserved.
  //
  // Known limitation: when the subtree contains mandatory-promoted
  // sublayers (filter groups, clip-path groups), those entities are
  // still iterated by `drawEntityRange` and burned into this layer's
  // bitmap AS WELL AS into their own. The double-draw produces
  // crescent-shaped drift at gradient-orb tops on
  // `#Clouds_with_gradients` drag. Tracked for a follow-up — fix
  // requires making `drawEntityRange` layer-aware or refusing
  // user-promotion when descendants are already promoted.
  using TreeComponent = donner::components::TreeComponent;
  const auto findLastPaintingDescendant = [&registry](auto& self, Entity start) -> Entity {
    Entity result =
        registry.all_of<components::RenderingInstanceComponent>(start) ? start : entt::null;
    const auto* tree = registry.try_get<TreeComponent>(start);
    if (tree == nullptr) return result;
    for (Entity child = tree->firstChild(); child != entt::null;) {
      const Entity descendantLast = self(self, child);
      if (descendantLast != entt::null) {
        result = descendantLast;
      }
      child = registry.get<TreeComponent>(child).nextSibling();
    }
    return result;
  };
  const Entity lastPainting = findLastPaintingDescendant(findLastPaintingDescendant, entity);
  return {entity, lastPainting != entt::null ? lastPainting : entity};
}

void CompositorController::propagateFastPathTranslationToSubtree(
    Registry& registry, Entity root, const Transform2d& worldFromPreviousWorld) {
  using TreeComponent = donner::components::TreeComponent;
  std::vector<Entity> stack;
  if (const auto* tree = registry.try_get<TreeComponent>(root)) {
    for (Entity child = tree->firstChild(); child != entt::null;
         child = registry.get<TreeComponent>(child).nextSibling()) {
      stack.push_back(child);
    }
  }
  while (!stack.empty()) {
    const Entity descendant = stack.back();
    stack.pop_back();

    if (auto* instance = registry.try_get<components::RenderingInstanceComponent>(descendant)) {
      // `worldFromEntity` maps entity-local points to world space. Since
      // only the root's local transform changed — by a pure world-space
      // translation — every descendant's world transform pre-multiplies
      // by that same delta (root-from-descendant is unchanged, world-
      // from-root shifts by delta, so world-from-descendant shifts too).
      instance->worldFromEntityTransform =
          worldFromPreviousWorld * instance->worldFromEntityTransform;
      // Invalidate the cached absolute-transform so any later
      // `LayoutSystem::getAbsoluteTransformComponent` query recomputes
      // from the authoritative DOM instead of returning the pre-drag
      // cache. Without this, a subsequent fast-path frame rooted at a
      // descendant (e.g. user clicks inside the dragged group after
      // dropping it) would compute a delta against the stale cached
      // transform and pick the wrong branch.
      registry.remove<components::ComputedAbsoluteTransformComponent>(descendant);
    }

    if (const auto* dtree = registry.try_get<TreeComponent>(descendant)) {
      for (Entity grandchild = dtree->firstChild(); grandchild != entt::null;
           grandchild = registry.get<TreeComponent>(grandchild).nextSibling()) {
        stack.push_back(grandchild);
      }
    }
  }
}

FallbackReason CompositorController::detectFallbackReasons(
    const components::RenderingInstanceComponent& instance) {
  FallbackReason reasons = FallbackReason::None;

  // Isolated layers (opacity < 1, isolation groups) interact with the backdrop.
  if (instance.isolatedLayer) {
    reasons |= FallbackReason::IsolatedLayer;
  }

  // Filters may reference BackgroundImage or other backdrop-dependent inputs.
  if (instance.resolvedFilter.has_value()) {
    reasons |= FallbackReason::Filter;
  }

  // Clip paths reference elements that may be outside the promoted subtree.
  if (instance.clipPath.has_value()) {
    reasons |= FallbackReason::ClipPath;
  }

  // Masks reference elements that may be outside the promoted subtree.
  if (instance.mask.has_value()) {
    reasons |= FallbackReason::Mask;
  }

  // Markers depend on the containing document context.
  if (instance.markerStart.has_value() || instance.markerMid.has_value() ||
      instance.markerEnd.has_value()) {
    reasons |= FallbackReason::Markers;
  }

  // Paint servers with resolved references (gradients/patterns) may reference external elements.
  if (std::holds_alternative<components::PaintResolvedReference>(instance.resolvedFill) ||
      std::holds_alternative<components::PaintResolvedReference>(instance.resolvedStroke)) {
    reasons |= FallbackReason::ExternalPaint;
  }

  return reasons;
}

}  // namespace donner::svg::compositor
