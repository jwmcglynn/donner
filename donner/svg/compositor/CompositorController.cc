#include "donner/svg/compositor/CompositorController.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg::compositor {

namespace {

/// Tracks entities whose visibility the compositor temporarily set to `false`
/// during a render pass. The post-pass restore unconditionally sets
/// `visible = true` (see `RestoreLayerVisibility`), so we only need to
/// remember *which* entities we touched — not their prior state.
struct HiddenVisibilityState {
  Entity entity;
};

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

void HideLayerRange(Registry& registry, const CompositorLayer& layer,
                    std::vector<HiddenVisibilityState>* hiddenEntities) {
  if (!registry.valid(layer.firstEntity()) || !registry.valid(layer.lastEntity())) {
    return;
  }

  RenderingInstanceView view(registry);
  while (!view.done() && view.currentEntity() != layer.firstEntity()) {
    view.advance();
  }

  while (!view.done()) {
    const Entity entity = view.currentEntity();
    auto& instance = registry.get<components::RenderingInstanceComponent>(entity);

    const auto alreadyHidden = std::find_if(
        hiddenEntities->begin(), hiddenEntities->end(),
        [entity](const HiddenVisibilityState& state) { return state.entity == entity; });
    if (alreadyHidden == hiddenEntities->end()) {
      hiddenEntities->push_back(HiddenVisibilityState{.entity = entity});
    }

    instance.visible = false;

    if (entity == layer.lastEntity()) {
      break;
    }

    view.advance();
  }
}

void HideEntitiesBefore(Registry& registry, Entity boundaryExclusive,
                        std::vector<HiddenVisibilityState>* hiddenEntities) {
  if (!registry.valid(boundaryExclusive)) {
    return;
  }

  RenderingInstanceView view(registry);
  while (!view.done() && view.currentEntity() != boundaryExclusive) {
    const Entity entity = view.currentEntity();
    auto& instance = registry.get<components::RenderingInstanceComponent>(entity);
    hiddenEntities->push_back(HiddenVisibilityState{.entity = entity});
    instance.visible = false;
    view.advance();
  }
}

void HideEntitiesAfter(Registry& registry, Entity boundaryInclusive,
                       std::vector<HiddenVisibilityState>* hiddenEntities) {
  if (!registry.valid(boundaryInclusive)) {
    return;
  }

  RenderingInstanceView view(registry);
  while (!view.done() && view.currentEntity() != boundaryInclusive) {
    view.advance();
  }

  if (view.done()) {
    return;
  }

  view.advance();
  while (!view.done()) {
    const Entity entity = view.currentEntity();
    auto& instance = registry.get<components::RenderingInstanceComponent>(entity);
    hiddenEntities->push_back(HiddenVisibilityState{.entity = entity});
    instance.visible = false;
    view.advance();
  }
}

void RestoreLayerVisibility(Registry& registry,
                            const std::vector<HiddenVisibilityState>& hiddenEntities) {
  // Always restore to `visible = true` for entities we hid. Reasoning:
  //
  // `driver.draw()` inside a compositor render pass calls
  // `prepareDocumentForRendering`, which rebuilds
  // `RenderingInstanceComponent`s with fresh defaults (`visible = true`).
  // If we restore using a *stale* saved value instead, we can stomp the
  // freshly-rebuilt true with a leaked false from a prior pass — leaving
  // entities permanently hidden across subsequent frames. We only ever hide
  // user-visible promoted content, so unconditional `true` is correct for the
  // compositor's use case.
  for (const auto& hidden : hiddenEntities) {
    if (!registry.valid(hidden.entity)) {
      continue;
    }
    auto* inst = registry.try_get<components::RenderingInstanceComponent>(hidden.entity);
    if (inst == nullptr) {
      continue;
    }
    inst->visible = true;
  }
}

}  // namespace

CompositorController::CompositorController(SVGDocument& document, RendererInterface& renderer,
                                           CompositorConfig config)
    : document_(&document),
      renderer_(&renderer),
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

bool CompositorController::promoteEntity(Entity entity, InteractionHint interactionKind) {
  Registry& registry = document_->registry();

  if (!registry.valid(entity)) {
    return false;
  }

  // Refuse promotion if an ancestor has a filter / mask / clip-path. Extracting
  // the descendant into its own cached layer would lose the ancestor's
  // compositing context — the user would see, e.g., the blur disappear while
  // dragging a path that lives inside `<g filter="url(#blur)">`. The caller
  // (editor) falls back to the non-composited drag path which renders
  // correctly via the full tree walk.
  if (HasCompositingBreakingAncestor(registry, entity)) {
    return false;
  }

  // Already promoted via the controller's `promoteEntity` path. `activeHints_`
  // is the controller-owned hint ledger (Interaction under
  // `autoPromoteInteractions`, Explicit otherwise). A future Mandatory or
  // Animation hint from another source would also produce a
  // `ComputedLayerAssignmentComponent` but doesn't count as "promoted by the
  // controller" here.
  if (activeHints_.contains(entity)) {
    return true;
  }

  if (activeHints_.size() >= static_cast<size_t>(kMaxCompositorLayers)) {
    return false;
  }

  if (totalBitmapMemory() >= kMaxCompositorMemoryBytes) {
    return false;
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
    return false;
  }

  rootDirty_ = true;
  return true;
}

void CompositorController::demoteEntity(Entity entity) {
  Registry& registry = document_->registry();

  const auto hintIt = activeHints_.find(entity);
  if (hintIt != activeHints_.end()) {
    activeHints_.erase(hintIt);
  }

  resolver_.resolve(registry, kMaxCompositorLayers);
  reconcileLayers(registry);

  // `reconcileLayers` marks `rootDirty_` when it removes a layer, but a demote
  // on an entity that was never promoted should still clear the split-bitmap
  // caches to match prior semantics.
  backgroundBitmap_ = RendererBitmap();
  foregroundBitmap_ = RendererBitmap();
  splitStaticLayersEntity_ = entt::null;
  splitStaticLayersViewport_ = Vector2i::Zero();
  rootDirty_ = true;
}

bool CompositorController::isPromoted(Entity entity) const {
  return activeHints_.contains(entity);
}

void CompositorController::setLayerCompositionTransform(Entity entity,
                                                        const Transform2d& transform) {
  CompositorLayer* layer = findLayer(entity);
  if (!layer) {
    return;
  }

  layer->setCompositionTransform(transform);

  // Non-translation transforms require re-rasterization.
  if (!transform.isTranslation()) {
    layer->markDirty();
  }
}

Transform2d CompositorController::compositionTransformOf(Entity entity) const {
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->compositionTransform() : Transform2d();
}

FallbackReason CompositorController::fallbackReasonsOf(Entity entity) const {
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->fallbackReasons() : FallbackReason::None;
}

bool CompositorController::hasSplitStaticLayers() const {
  // The split-static-layers optimization (background / promoted-drag / foreground) depends on
  // having exactly ONE moving layer — the drag target. Bucket layers (from
  // `ComplexityBucketer`) stay static during a drag, so they don't invalidate the split as long
  // as there's exactly one active-hint (drag/explicit) entry. Check `activeHints_`, not
  // `layers_.size()`, so bucketing and drag coexist cleanly.
  return activeHints_.size() == 1 &&
         (!backgroundBitmap_.empty() || !foregroundBitmap_.empty());
}

const RendererBitmap& CompositorController::layerBitmapOf(Entity entity) const {
  static const RendererBitmap kEmptyBitmap;
  const CompositorLayer* layer = findLayer(entity);
  return layer ? layer->bitmap() : kEmptyBitmap;
}

void CompositorController::resetAllLayers() {
  Registry& registry = document_->registry();
  activeHints_.clear();
  // Drop auto-promoted hints too — they index the old entity space and must
  // be rebuilt against the new document on the next `renderFrame`.
  mandatoryDetector_.clear();
  complexityBucketer_.clear();
  resolver_.resolve(registry, kMaxCompositorLayers);
  reconcileLayers(registry);

  rootBitmap_ = RendererBitmap();
  backgroundBitmap_ = RendererBitmap();
  foregroundBitmap_ = RendererBitmap();
  splitStaticLayersEntity_ = entt::null;
  splitStaticLayersViewport_ = Vector2i::Zero();
  rootDirty_ = true;
  documentPrepared_ = false;
  hintsScanned_ = false;
}

void CompositorController::renderFrame(const RenderViewport& viewport) {
  UTILS_RELEASE_ASSERT(document_ != nullptr);
  UTILS_RELEASE_ASSERT(renderer_ != nullptr);

  Registry& registry = document_->registry();

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
  {
    auto dirtyView = registry.view<components::DirtyFlagsComponent>();
    for (const Entity entity : dirtyView) {
      const auto& dirty = dirtyView.get<components::DirtyFlagsComponent>(entity);
      if (dirty.flags != components::DirtyFlagsComponent::Flags::None) {
        dirtyEntitySnapshot.push_back(entity);
      }
    }
  }
  const bool anyEntityDirty = !dirtyEntitySnapshot.empty();
  const bool needsFullRebuild =
      registry.ctx().contains<components::RenderTreeState>() &&
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
  // rebuild entirely: just refresh their `entityFromWorldTransform` from
  // the freshly-computed `ComputedAbsoluteTransformComponent` (invalidated
  // in place by `LayoutSystem::setTransform`) and mark the layer dirty.
  // Everything else — filter layer bitmaps, bg/fg split, style cascade —
  // stays untouched across the mutation.
  bool tookFastTransformPath = false;
  if (documentPrepared_ && !needsFullRebuild && !dirtyEntitySnapshot.empty() &&
      hintsScanned_) {
    constexpr uint16_t kTransformOnlyMask =
        components::DirtyFlagsComponent::Layout |
        components::DirtyFlagsComponent::Transform |
        components::DirtyFlagsComponent::WorldTransform |
        components::DirtyFlagsComponent::RenderInstance;

    bool eligible = true;
    for (Entity e : dirtyEntitySnapshot) {
      const auto* dirty = registry.try_get<components::DirtyFlagsComponent>(e);
      if (dirty == nullptr) continue;
      if ((dirty->flags & ~kTransformOnlyMask) != 0) {
        eligible = false;
        break;
      }
      // The entity must be a single-element promoted layer root — if the
      // layer has a subtree, descendants' `entityFromWorldTransform`
      // would also need refreshing, and we punt that to the slow path.
      bool matchedSingleEntityLayer = false;
      for (auto& layer : layers_) {
        if (layer.entity() == e && layer.firstEntity() == e && layer.lastEntity() == e) {
          matchedSingleEntityLayer = true;
          break;
        }
      }
      if (!matchedSingleEntityLayer) {
        eligible = false;
        break;
      }
    }

    if (eligible) {
      for (Entity e : dirtyEntitySnapshot) {
        if (!registry.all_of<components::RenderingInstanceComponent>(e)) continue;
        auto& instance = registry.get<components::RenderingInstanceComponent>(e);
        // `LayoutSystem::setTransform` already removed the stale
        // `ComputedAbsoluteTransformComponent`, so this recomputes.
        const auto& abs = components::LayoutSystem().getAbsoluteTransformComponent(
            EntityHandle(registry, e));
        // Canvas-space transform, matching the original computation in
        // `RenderingContext::traverseTree`.
        const Transform2d canvasFromDocument =
            components::LayoutSystem().getCanvasFromDocumentTransform(registry);
        instance.entityFromWorldTransform =
            abs.entityFromWorld * (abs.worldIsCanvas ? canvasFromDocument : Transform2d());
        for (auto& layer : layers_) {
          if (layer.entity() == e) layer.markDirty();
        }
      }
      // Clear the dirty flags ourselves since we skipped prepare.
      registry.clear<components::DirtyFlagsComponent>();
      tookFastTransformPath = true;
      // Tell the rest of renderFrame that the dirty state is fully resolved
      // — no need to re-run detectors, re-prepare, or re-resolve. We'll
      // still fall through to the layer-rasterize loop, which will pick up
      // the `markDirty` we just set and re-rasterize only the affected
      // drag layer.
      documentDirty = false;
    }
  }
  // On first renderFrame the `RenderingInstanceComponent` view is empty (it
  // gets built by `prepareDocumentForRendering` below), so the mandatory /
  // bucket detectors can't score anything useful. Defer them to the first
  // post-prepare frame by also rescanning whenever `hintsScanned_` is false.
  const bool needsHintRescan = !hintsScanned_ || documentDirty;
  if ((!documentPrepared_ || documentDirty) && documentPrepared_) {
    // Document is already prepared and became dirty — rescan now.
    mandatoryDetector_.reconcile(registry);
    if (config_.complexityBucketing) {
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
  resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
  reconcileLayers(registry);
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

  // If no promoted layers, take the simple full-render path. This also runs
  // `prepareDocumentForRendering` implicitly via `driver.draw()`, so by the
  // time the next `renderFrame` sees the document the detectors can actually
  // observe RICs.
  if (layers_.empty()) {
    RendererDriver driver(*renderer_);
    driver.draw(*document_);
    backgroundBitmap_ = RendererBitmap();
    foregroundBitmap_ = RendererBitmap();
    splitStaticLayersEntity_ = entt::null;
    splitStaticLayersViewport_ = Vector2i::Zero();
    rootDirty_ = false;
    documentPrepared_ = true;

    // Populated RICs now exist. If the detectors haven't scanned yet, run
    // them once against the populated view and rebuild `layers_`. A newly
    // discovered mandatory filter/mask/isolated-layer promotes the affected
    // subtree immediately so the NEXT render sees a populated `layers_` and
    // uses the compositor path. We don't re-render this frame — the simple
    // full render we just produced is correct, it just doesn't benefit from
    // caching until the next call.
    if (!hintsScanned_) {
      mandatoryDetector_.reconcile(registry);
      if (config_.complexityBucketing) {
        complexityBucketer_.reconcile(registry);
      }
      hintsScanned_ = true;
      resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
      reconcileLayers(registry);
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
    ParseWarningSink warningSink;
    RendererUtils::prepareDocumentForRendering(*document_, /*verbose=*/false, warningSink);
    documentPrepared_ = true;
    refreshLayerMetadata();

    // After preparation, clear the needsFullRebuild flag so consumeDirtyFlags doesn't
    // re-trigger a full rebuild on the next frame. The render tree instantiation process
    // leaves needsFullRebuild=true as a side effect of invalidateRenderTree(); we consume
    // that signal here.
    if (registry.ctx().contains<components::RenderTreeState>()) {
      registry.ctx().get<components::RenderTreeState>().needsFullRebuild = false;
    }

    // First prepare completed — run detectors now if they haven't yet.
    if (!hintsScanned_) {
      mandatoryDetector_.reconcile(registry);
      if (config_.complexityBucketing) {
        complexityBucketer_.reconcile(registry);
      }
      hintsScanned_ = true;
      resolver_.resolve(registry, kMaxCompositorLayers, resolveOptions);
      reconcileLayers(registry);
    }
  }

  if (!offscreenSupportKnown_) {
    offscreenSupported_ = renderer_->createOffscreenInstance() != nullptr;
    offscreenSupportKnown_ = true;
  }

  if (!offscreenSupported_) {
    RendererDriver driver(*renderer_);
    driver.draw(*document_);
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
  consumeDirtyFlags(dirtyEntitySnapshot);

  // Re-rasterize dirty promoted layers. Dirty tracking subsumes what
  // `requiresConservativeFallback()` used to trigger here: the explicit
  // "conservative" invalidation above now flips `isDirty()` when the
  // document actually changed, so we don't need to force re-rasterization
  // of conservative-fallback layers on every frame. `rootDirty_` is
  // reserved for full-tree rebuilds; individual entity mutations flow
  // through `consumeDirtyFlags(dirtyEntitySnapshot)`. This is what keeps
  // drag-release → drag-again on the same entity from re-rasterizing
  // every mandatory filter layer.
  for (auto& layer : layers_) {
    if (layer.isDirty() || !layer.hasValidBitmap() || rootDirty_) {
      rasterizeLayer(layer, viewport);
    }
  }

  // Re-rasterize root (or split bg/fg) when dirty. In the split path the
  // `rootBitmap_` stays empty by design and the validity of the cache is
  // tracked separately inside `rasterizeRootLayer` — so don't use
  // `rootBitmap_.empty()` alone as a trigger, or we'd re-rasterize bg/fg
  // on every drag frame. The cache is also invalidated when the canvas
  // size changes (zoom / window resize).
  const bool splitPathActive = activeHints_.size() == 1 && findLayer(activeHints_.begin()->first);
  const Vector2i currentCanvasSize = document_->canvasSize();
  const bool needsRootRasterize =
      rootDirty_ || (!splitPathActive && rootBitmap_.empty()) ||
      (splitPathActive &&
       (backgroundBitmap_.empty() || foregroundBitmap_.empty() ||
        splitStaticLayersEntity_ != activeHints_.begin()->first ||
        splitStaticLayersViewport_ != currentCanvasSize));
  if (needsRootRasterize) {
    rasterizeRootLayer(viewport);
  }

  // Compose all layers onto the main target.
  composeLayers(viewport);

  // Dual-path pixel-identity assertion. When enabled, capture the composited
  // output, run a full-document reference render to the same renderer, then
  // compare pixel-by-pixel. Any drift fires a release-assert. Disabled by
  // default; CI compositor test targets flip this on via `CompositorConfig`.
  // Cost is 2x per frame, so don't enable for interactive editor use.
  if (config_.verifyPixelIdentity) {
    RendererBitmap compositedSnapshot = renderer_->takeSnapshot();

    RendererDriver referenceDriver(*renderer_);
    referenceDriver.draw(*document_);
    RendererBitmap referenceSnapshot = renderer_->takeSnapshot();

    if (compositedSnapshot.dimensions == referenceSnapshot.dimensions &&
        !compositedSnapshot.empty() && !referenceSnapshot.empty()) {
      size_t mismatch = 0;
      uint8_t maxDiff = 0;
      const size_t size = std::min(compositedSnapshot.pixels.size(),
                                   referenceSnapshot.pixels.size());
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
  size_t total =
      rootBitmap_.pixels.size() + backgroundBitmap_.pixels.size() + foregroundBitmap_.pixels.size();
  for (const auto& layer : layers_) {
    total += layer.bitmap().pixels.size();
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
  auto offscreen = renderer_->createOffscreenInstance();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);

  Registry& registry = document_->registry();

  RendererDriver driver(*offscreen);
  driver.drawEntityRange(registry, layer.firstEntity(), layer.lastEntity(), viewport,
                         Transform2d());

  layer.setBitmap(offscreen->takeSnapshot());
}

void CompositorController::rasterizeRootLayer(const RenderViewport& viewport) {
  // If there's exactly one drag layer (single entity in `activeHints_`), use the split-static-
  // layers optimization regardless of how many bucket layers exist. The split is around the drag
  // layer's draw order; bucket layers stay static and are drawn separately via `composeLayers`.
  if (activeHints_.size() == 1) {
    const Entity dragEntity = activeHints_.begin()->first;
    const CompositorLayer* dragLayer = findLayer(dragEntity);
    if (dragLayer != nullptr) {
      // Cache bg/fg across drag frames: re-rasterize only when the drag
      // target changes, the document becomes dirty (`rootDirty_`), or the
      // document's canvas size changes (zoom / window resize). A canvas
      // change means the renderer rasterizes at a new resolution, so stale
      // bg/fg bitmaps at the old size would draw into only the top-left
      // region of the new canvas — the rest would show through with
      // transparency, and the editor's ImGui layer would then linearly
      // stretch whatever did draw, which shows as a blurry image on zoom.
      //
      // We key on `document.canvasSize()` (not the `RenderViewport` param)
      // because `rasterizeSplitRootLayers` internally calls
      // `RendererDriver::draw(document)`, which uses `document.canvasSize()`
      // for the offscreen frame buffer size — that's what the bitmap
      // dimensions actually reflect.
      const Vector2i currentCanvas = document_->canvasSize();
      const bool splitCacheValid =
          !rootDirty_ && !backgroundBitmap_.empty() && !foregroundBitmap_.empty() &&
          splitStaticLayersEntity_ == dragEntity &&
          splitStaticLayersViewport_ == currentCanvas;
      if (!splitCacheValid) {
        rasterizeSplitRootLayers(*dragLayer, viewport);
        splitStaticLayersEntity_ = dragEntity;
        splitStaticLayersViewport_ = currentCanvas;
      }
      rootBitmap_ = RendererBitmap();
      rootDirty_ = false;
      return;
    }
  }

  // Leaving the split path — drop the cache identity so re-entering the
  // split path with the same drag entity (or a different one) rebuilds bg/fg
  // against the current document state.
  splitStaticLayersEntity_ = entt::null;
  splitStaticLayersViewport_ = Vector2i::Zero();

  // Render the root layer with promoted subtrees temporarily hidden so compositor translation
  // doesn't leave a stale copy at the original position.
  auto offscreen = renderer_->createOffscreenInstance();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);

  Registry& registry = document_->registry();
  std::vector<HiddenVisibilityState> hiddenEntities;
  hiddenEntities.reserve(layers_.size());
  for (const auto& layer : layers_) {
    HideLayerRange(registry, layer, &hiddenEntities);
  }

  RendererDriver driver(*offscreen);
  driver.draw(*document_);
  RestoreLayerVisibility(registry, hiddenEntities);
  rootBitmap_ = offscreen->takeSnapshot();

  backgroundBitmap_ = RendererBitmap();
  foregroundBitmap_ = RendererBitmap();
  rootDirty_ = false;
}

void CompositorController::rasterizeSplitRootLayers(const CompositorLayer& layer,
                                                    const RenderViewport& viewport) {
  Registry& registry = document_->registry();

  auto renderMaskedDocument = [&](auto hideFn) {
    auto offscreen = renderer_->createOffscreenInstance();
    UTILS_RELEASE_ASSERT(offscreen != nullptr);

    std::vector<HiddenVisibilityState> hiddenEntities;
    hideFn(&hiddenEntities);

    RendererDriver driver(*offscreen);
    driver.draw(*document_);
    RestoreLayerVisibility(registry, hiddenEntities);
    return offscreen->takeSnapshot();
  };

  // Hide *only* the drag layer in bg/fg — mandatory filter / bucket layers
  // must render inline so they paint in correct paint order relative to
  // surrounding regular content. Extracting them and drawing all "other
  // promoted layers" between bg and fg in `composeLayers` would stack a
  // filter that paints *after* the drag target beneath whatever fg draws on
  // top of it, inverting paint order for overlapping sibling content (e.g. a
  // glow that should cover the letter alongside it).
  const auto hideDragLayer = [&](std::vector<HiddenVisibilityState>* hiddenEntities) {
    HideLayerRange(registry, layer, hiddenEntities);
  };

  backgroundBitmap_ = renderMaskedDocument([&](std::vector<HiddenVisibilityState>* hiddenEntities) {
    hideDragLayer(hiddenEntities);
    HideEntitiesAfter(registry, layer.lastEntity(), hiddenEntities);
  });

  foregroundBitmap_ = renderMaskedDocument([&](std::vector<HiddenVisibilityState>* hiddenEntities) {
    hideDragLayer(hiddenEntities);
    HideEntitiesBefore(registry, layer.firstEntity(), hiddenEntities);
  });
}

namespace {

/// Convert premultiplied-alpha pixels to unpremultiplied-alpha pixels in
/// place. Required before passing a `RendererBitmap` snapshot (which is
/// premultiplied by convention) into `RendererInterface::drawImage` via
/// `ImageResource` (which has no alpha-type field and is implicitly
/// unpremultiplied). Without this conversion, semi-transparent pixels compose
/// as though their RGB were already multiplied by alpha a *second* time —
/// showing up as too-dark colors for opacity<1 content.
///
/// Fully-opaque pixels (alpha = 255) are unchanged; this step is only
/// meaningful for semi-transparent content, which is why single-element
/// drag of fully-opaque elements worked without the fix.
std::vector<uint8_t> UnpremultiplyPixels(const std::vector<uint8_t>& src) {
  std::vector<uint8_t> dst(src.size());
  for (size_t i = 0; i + 3 < src.size(); i += 4) {
    const uint8_t a = src[i + 3];
    if (a == 0) {
      dst[i] = 0;
      dst[i + 1] = 0;
      dst[i + 2] = 0;
      dst[i + 3] = 0;
    } else if (a == 255) {
      dst[i] = src[i];
      dst[i + 1] = src[i + 1];
      dst[i + 2] = src[i + 2];
      dst[i + 3] = 255;
    } else {
      // Divide RGB by alpha normalized to [0, 1]. Integer math: round half up.
      const uint32_t scale = 255 * 256 / a;  // inverse alpha * 256
      dst[i] = static_cast<uint8_t>(std::min<uint32_t>(255, (src[i] * scale + 128) >> 8));
      dst[i + 1] = static_cast<uint8_t>(std::min<uint32_t>(255, (src[i + 1] * scale + 128) >> 8));
      dst[i + 2] = static_cast<uint8_t>(std::min<uint32_t>(255, (src[i + 2] * scale + 128) >> 8));
      dst[i + 3] = a;
    }
  }
  return dst;
}

/// Build an `ImageResource` from a `RendererBitmap`, converting from
/// premultiplied to unpremultiplied alpha when necessary. See
/// `UnpremultiplyPixels` for the why.
ImageResource BuildImageResource(const RendererBitmap& bitmap) {
  ImageResource img;
  img.width = bitmap.dimensions.x;
  img.height = bitmap.dimensions.y;
  if (bitmap.alphaType == AlphaType::Premultiplied) {
    img.data = UnpremultiplyPixels(bitmap.pixels);
  } else {
    img.data = bitmap.pixels;
  }
  return img;
}

}  // namespace

void CompositorController::composeLayers(const RenderViewport& viewport) {
  renderer_->beginFrame(viewport);

  const auto drawBitmap = [this](const RendererBitmap& bitmap, const Transform2d& transform) {
    if (bitmap.empty()) {
      return;
    }
    ImageResource image = BuildImageResource(bitmap);
    ImageParams params;
    params.targetRect =
        Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(bitmap.dimensions.x),
                                         static_cast<double>(bitmap.dimensions.y)));
    renderer_->setTransform(transform);
    renderer_->drawImage(image, params);
  };

  const auto drawLayer = [&](const CompositorLayer& layer) {
    if (!layer.hasValidBitmap()) {
      return;
    }
    // Apply composition transform with integer-pixel snap for translations.
    Transform2d compositionTransform = layer.compositionTransform();
    if (compositionTransform.isTranslation()) {
      Vector2d t = compositionTransform.translation();
      compositionTransform = Transform2d::Translate(std::round(t.x), std::round(t.y));
    }
    drawBitmap(layer.bitmap(), compositionTransform);
  };

  if (hasSplitStaticLayers()) {
    // Split-static-layers path: bg and fg include all non-drag-target content
    // rendered inline (including any mandatory filter / bucket layers, so
    // their draw order relative to surrounding regular content is preserved).
    // Only the drag target is composed as a separate cached layer.
    const Entity dragEntity = activeHints_.begin()->first;
    drawBitmap(backgroundBitmap_, Transform2d());
    if (const CompositorLayer* dragLayer = findLayer(dragEntity)) {
      drawLayer(*dragLayer);
    }
    drawBitmap(foregroundBitmap_, Transform2d());
  } else {
    // No drag — single root bitmap plus any promoted (mandatory / bucket)
    // layers composed in paint-order order. `rasterizeRootLayer` hid the
    // promoted layers' content from `rootBitmap_`, so drawing them here
    // doesn't double-paint.
    drawBitmap(rootBitmap_, Transform2d());
    for (const auto& layer : layers_) {
      drawLayer(layer);
    }
  }

  renderer_->endFrame();
}

void CompositorController::consumeDirtyFlags(const std::vector<Entity>& dirtyEntities) {
  Registry& registry = document_->registry();

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
      rootDirty_ = true;
    }
  }

  // Global render tree state.
  if (registry.ctx().contains<components::RenderTreeState>()) {
    auto& state = registry.ctx().get<components::RenderTreeState>();
    if (state.needsFullRebuild) {
      rootDirty_ = true;
      for (auto& layer : layers_) {
        layer.markDirty();
      }
    }
  }
}

void CompositorController::refreshLayerMetadata() {
  Registry& registry = document_->registry();
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
  const auto removeIt = std::remove_if(
      layers_.begin(), layers_.end(),
      [&registry, this](const CompositorLayer& layer) {
        if (!registry.valid(layer.entity())) {
          rootDirty_ = true;
          return true;
        }
        const auto* assignment =
            registry.try_get<ComputedLayerAssignmentComponent>(layer.entity());
        if (assignment == nullptr || assignment->layerId == 0) {
          rootDirty_ = true;
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

    rootDirty_ = true;
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
  Registry& registry = document_->registry();
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

  return {entity, entity};
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
