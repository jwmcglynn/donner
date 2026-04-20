#include "donner/svg/compositor/CompositorController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
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
ImageResource BuildImageResource(const RendererBitmap& bitmap) {
  ImageResource img;
  img.width = bitmap.dimensions.x;
  img.height = bitmap.dimensions.y;
  if (bitmap.alphaType == AlphaType::Premultiplied) {
    img.data = UnpremultiplyRgba(bitmap.pixels);
  } else {
    img.data = bitmap.pixels;
  }
  return img;
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
      const auto* assignment =
          registry.try_get<ComputedLayerAssignmentComponent>(descendant);
      if (assignment != nullptr && assignment->layerId != 0) {
        return false;
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
    return false;
  }

  // Don't force a full cache invalidation — `resyncSegmentsToLayerSet`
  // preserves segments whose boundary identity survives the new layer
  // insertion, which is what keeps click-to-first-pixel fast on the
  // splash's first drag-target promote.
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

  // The split bg / drag / fg cache is keyed on the drag entity itself —
  // once the drag target goes away, it's meaningless and must be
  // dropped. Skipping this would leave a stale bg/fg pair carrying the
  // previous entity's composition, which the editor would then upload
  // and draw against the new selection.
  backgroundBitmap_ = RendererBitmap();
  foregroundBitmap_ = RendererBitmap();
  splitStaticLayersEntity_ = entt::null;
  splitStaticLayersViewport_ = Vector2i::Zero();

  // Do NOT clear `staticSegments_` / set `rootDirty_` — those would
  // nuke every cached segment bitmap on every demote, which turns
  // "release D then click O" into a multi-second freeze as the
  // compositor re-rasterizes all 9 segments + every mandatory filter
  // layer from scratch. The next `renderFrame` will call
  // `resyncSegmentsToLayerSet`, which surgically preserves every
  // segment whose boundary pair survived the layer removal and marks
  // only the slot(s) whose boundaries actually changed (the two that
  // used to border the demoted layer collapse into one). `reconcile
  // Layers` above already preserved the promoted-layer bitmaps that
  // didn't change. See the `MultiShapeClickDragHiDpiRepro` test for
  // the gate.
}

bool CompositorController::isPromoted(Entity entity) const {
  return activeHints_.contains(entity);
}

Transform2d CompositorController::layerComposeOffset(Entity entity) const {
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
  Registry& registry = document_->registry();

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
  RendererUtils::prepareDocumentForRendering(*document_, /*verbose=*/false, warningSink);
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

  // Step 2: rebuild the auto-promotion detectors against the new
  // registry. `prepareDocumentForRendering` above populated RICs, so
  // `reconcile` has the data it needs to re-detect filter / bucket
  // layers and publish fresh hints keyed on the new entity ids.
  mandatoryDetector_.rebuildForReplacedDocument(registry);
  if (config_.complexityBucketing) {
    complexityBucketer_.rebuildForReplacedDocument(registry);
  }
  hintsScanned_ = true;

  // Step 3: remap layer entity ids. Cached `bitmap_`, `composition
  // Transform_`, `bitmapEntityFromWorldTransform_`, and
  // `fallbackReasons_` are preserved — they were valid for the old
  // entity at its paint-order slot and remain valid for the new entity
  // at the same slot.
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
      // Cache identity lost — next render will recompute bg/fg.
      backgroundBitmap_ = RendererBitmap();
      foregroundBitmap_ = RendererBitmap();
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
  refreshLayerMetadata();

  return true;
}

void CompositorController::resetAllLayers(bool documentReplaced) {
  Registry& registry = document_->registry();
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
  resolver_.resolve(registry, kMaxCompositorLayers);
  reconcileLayers(registry);

  staticSegments_.clear();
  staticSegmentBoundaries_.clear();
  staticSegmentDirty_.clear();
  staticSegmentGeneration_.clear();
  staticSegmentOffsets_.clear();
  staticSegmentsCanvas_ = Vector2i::Zero();
  staticSegmentsLayerCount_ = 0;
  backgroundBitmap_ = RendererBitmap();
  foregroundBitmap_ = RendererBitmap();
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

void CompositorController::renderFrame(const RenderViewport& viewport) {
  ZoneScopedN("Compositor::renderFrame");
  UTILS_RELEASE_ASSERT(document_ != nullptr);
  UTILS_RELEASE_ASSERT(renderer_ != nullptr);

  // Slow-frame diagnostic: when a single renderFrame takes >1s, dump a
  // state summary + per-layer rasterize reasons to stderr so we can
  // reconstruct a faithful UI-level repro test. Data is collected
  // throughout the function and only printed at the end (guarded by
  // elapsed time) so normal fast frames pay near-zero overhead.
  const auto slowFrameStart = std::chrono::steady_clock::now();
  struct SlowFrameRasterReason {
    Entity entity = entt::null;
    bool isDirty = false;
    bool hasValidBitmap = false;
    bool rootDirtyTrigger = false;
    size_t layerIndex = 0;
  };
  std::vector<SlowFrameRasterReason> slowFrameRasterLog;
  size_t slowFrameSegmentDirtyCountAtStart = 0;
  bool slowFrameRootDirtyAtStart = rootDirty_;
  bool slowFrameNeedsFullRebuildAtStart = false;

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
  const bool needsFullRebuild =
      registry.ctx().contains<components::RenderTreeState>() &&
      registry.ctx().get<components::RenderTreeState>().needsFullRebuild;
  slowFrameNeedsFullRebuildAtStart = needsFullRebuild;
  for (bool d : staticSegmentDirty_) {
    if (d) ++slowFrameSegmentDirtyCountAtStart;
  }
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
  if (documentPrepared_ && !needsFullRebuild && !dirtyEntitySnapshot.empty() &&
      hintsScanned_) {
    constexpr uint16_t kTransformOnlyMask =
        components::DirtyFlagsComponent::Layout |
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
      Transform2d delta;
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

      // Dirty entity must be a promoted layer's root (so its RIC owns the
      // layer's `bitmapEntityFromWorldTransform` anchor). The old check
      // additionally required `firstEntity == lastEntity == e` —
      // rejecting every subtree layer and forcing a full
      // `prepareDocumentForRendering` on every drag frame. For filter
      // groups and other subtree-promoted roots on the splash that cost
      // ~40 ms/frame; the relaxation below brings it back under 5 ms by
      // running the same compose-offset update and lifting the
      // translation into descendants via `propagateFastPathTranslation
      // ToSubtree`.
      CompositorLayer* matchedLayer = nullptr;
      for (auto& layer : layers_) {
        if (layer.entity() == e) {
          matchedLayer = &layer;
          break;
        }
      }
      if (matchedLayer == nullptr || !matchedLayer->hasValidBitmap() ||
          !matchedLayer->bitmapEntityFromWorldTransform().has_value()) {
        eligible = false;
        break;
      }

      const auto& abs = components::LayoutSystem().getAbsoluteTransformComponent(
          EntityHandle(registry, e));
      const Transform2d canvasFromDocument =
          components::LayoutSystem().getCanvasFromDocumentTransform(registry);
      const Transform2d newWorldFromEntity =
          abs.worldFromEntity * (abs.worldIsCanvas ? canvasFromDocument : Transform2d());
      const Transform2d delta =
          newWorldFromEntity * matchedLayer->bitmapEntityFromWorldTransform()->inverse();

      const bool isSubtree =
          matchedLayer->firstEntity() != e || matchedLayer->lastEntity() != e;
      // Subtree layers require a pure-translation delta: only then can we
      // cheaply propagate the same offset to descendant RICs without
      // walking the layout system. Non-translation subtree mutations
      // (scale, rotate, transform-list change) stay on the slow path.
      // Single-entity layers can tolerate non-translation deltas via a
      // targeted re-rasterize later in `renderFrame`.
      if (isSubtree && !delta.isTranslation()) {
        eligible = false;
        break;
      }

      resolutions.push_back(FastPathResolution{
          .entity = e,
          .layer = matchedLayer,
          .newWorldFromEntity = newWorldFromEntity,
          .delta = delta,
          .isSubtree = isSubtree,
      });
    }

    if (eligible) {
      std::vector<Entity> unhandledDirtyEntities;
      unhandledDirtyEntities.reserve(resolutions.size());
      for (const auto& res : resolutions) {
        auto& instance =
            registry.get<components::RenderingInstanceComponent>(res.entity);
        instance.worldFromEntityTransform = res.newWorldFromEntity;

        if (res.delta.isTranslation()) {
          // Bitmap-reuse fast path: the delta between the current DOM
          // transform and the bitmap's rasterize-time transform is a
          // pure translation, so reuse the bitmap by updating
          // `compositionTransform_` instead of re-rasterizing. Every
          // mouse-move flips the entity's transform attribute, but the
          // compositor just writes a ~single-matrix compose offset
          // rather than going back to the renderer.
          res.layer->setCompositionTransform(res.delta);
          if (res.isSubtree) {
            propagateFastPathTranslationToSubtree(registry, res.entity, res.delta);
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
  const bool needsHintRescan = !hintsScanned_ || documentDirty;
  if ((!documentPrepared_ || documentDirty) && documentPrepared_) {
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
    RendererDriver driver(*renderer_);
    {
      ZoneScopedN("Compositor::driver.draw (first-frame)");
      driver.draw(*document_);
    }
    // `driver.draw` runs `prepareDocumentForRendering` → `instantiate
    // RenderTreeWithPrecomputedTree` which sets
    // `RenderTreeState::needsFullRebuild = true` as a side effect of
    // `invalidateRenderTree()` inside it. If we don't clear it here,
    // the NEXT `renderFrame` sees `needsFullRebuild=true`, flips
    // `rootDirty_=true`, and wipes every cached segment bitmap — which
    // turns the user's first click-to-drag into a multi-second freeze
    // because all the eager-warmed filter-layer segments blow away
    // and have to rebuild from scratch.
    if (registry.ctx().contains<components::RenderTreeState>()) {
      registry.ctx().get<components::RenderTreeState>().needsFullRebuild = false;
    }
    staticSegments_.clear();
    staticSegmentOffsets_.clear();
    staticSegmentsCanvas_ = Vector2i::Zero();
    staticSegmentsLayerCount_ = 0;
    backgroundBitmap_ = RendererBitmap();
    foregroundBitmap_ = RendererBitmap();
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
      if (offscreenSupported_ || (!offscreenSupportKnown_ &&
                                  renderer_->createOffscreenInstance() != nullptr)) {
        offscreenSupported_ = true;
        offscreenSupportKnown_ = true;
        {
          ZoneScopedN("Compositor::eagerWarmupRasterizeLayers");
          for (auto& layer : layers_) {
            if (!layer.hasValidBitmap()) {
              rasterizeLayer(layer, viewport);
            }
          }
        }
        const Vector2i currentCanvasSize = document_->canvasSize();
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
      RendererUtils::prepareDocumentForRendering(*document_, /*verbose=*/false, warningSink);
    }
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
  for (auto& layer : layers_) {
    if (!layer.hasValidBitmap() || !layer.bitmapEntityFromWorldTransform().has_value()) {
      continue;
    }
    if (!registry.valid(layer.entity()) ||
        !registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
      continue;
    }
    const auto& instance = registry.get<components::RenderingInstanceComponent>(layer.entity());
    const Transform2d& bitmapStamp = *layer.bitmapEntityFromWorldTransform();
    const Transform2d delta = instance.worldFromEntityTransform * bitmapStamp.inverse();
    if (delta.isTranslation()) {
      layer.setCompositionTransform(delta);
    }
    // Non-translation deltas (scale / rotate of an ancestor) require a
    // re-rasterize; `consumeDirtyFlags` / the rasterize loop handle
    // that via `layer.markDirty()`. We just silently leave those
    // layers alone here.
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
    size_t layerIndex = 0;
    for (auto& layer : layers_) {
      const bool layerDirty = layer.isDirty();
      const bool validBitmap = layer.hasValidBitmap();
      if (layerDirty || !validBitmap || rootDirty_) {
        // Capture the reason so the slow-frame log at the end of
        // `renderFrame` can explain why each layer rasterized. Three
        // possible triggers, listed in priority order: per-layer
        // `isDirty`, missing bitmap, or a global `rootDirty_` flag
        // (which re-rasterizes every layer).
        slowFrameRasterLog.push_back({
            .entity = layer.entity(),
            .isDirty = layerDirty,
            .hasValidBitmap = validBitmap,
            .rootDirtyTrigger = rootDirty_ && !layerDirty && validBitmap,
            .layerIndex = layerIndex,
        });
        rasterizeLayer(layer, viewport);
      }
      ++layerIndex;
    }
  }

  // Rasterize dirty static segments. `staticSegments_` holds one bitmap
  // per paint-order slot between promoted layers (plus the ends), and
  // `staticSegmentDirty_` tracks which slots need re-rasterization.
  // Structural changes (layer count, canvas size, `rootDirty_` from a
  // full tree rebuild) flip every slot dirty; per-entity mutations only
  // flip the containing slot dirty via `consumeDirtyFlags`.
  const Vector2i currentCanvasSize = document_->canvasSize();
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
    staticSegmentBoundaries_.clear();
    staticSegmentDirty_.clear();
    staticSegmentOffsets_.clear();
    staticSegmentsCanvas_ = Vector2i::Zero();
    staticSegmentsLayerCount_ = 0;
    backgroundBitmap_ = RendererBitmap();
    foregroundBitmap_ = RendererBitmap();
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
    // At least one segment changed; the bg/fg cache composed from the
    // old segment set is stale.
    backgroundBitmap_ = RendererBitmap();
    foregroundBitmap_ = RendererBitmap();
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
  // recomposite into offscreen bitmaps so non-drag promoted content
  // paints in its correct paint-order slot within bg/fg.
  const bool splitPathActive =
      activeHints_.size() == 1 && findLayer(activeHints_.begin()->first);
  if (splitPathActive) {
    const Entity dragEntity = activeHints_.begin()->first;
    const CompositorLayer* dragLayer = findLayer(dragEntity);
    const bool splitCacheValid =
        !backgroundBitmap_.empty() && !foregroundBitmap_.empty() &&
        splitStaticLayersEntity_ == dragEntity &&
        splitStaticLayersViewport_ == currentCanvasSize;
    if (dragLayer != nullptr && !splitCacheValid) {
      ZoneScopedN("Compositor::splitBitmapsBuild");
      // Always route through `recomposeSplitBitmaps` — it produces
      // canvas-sized bg/fg bitmaps that the editor uploads to GL
      // textures and blits at the full canvas screen rect. An earlier
      // "N=1 fast path" assigned `backgroundBitmap_ = staticSegments_[0]`
      // directly, which was valid when segments were always canvas-
      // sized but produces tight-sized bg/fg bitmaps now that segments
      // are tight-bounded. Those tight bitmaps get drawn stretched to
      // the full canvas screen rect, which wrecks the display. The
      // cost of always routing through `composeRange` on N=1 is one
      // offscreen alloc + one `drawImage` per segment (typically 2
      // segments, ~0.5 ms combined at 892×512). Worth the correctness.
      recomposeSplitBitmaps(*dragLayer, viewport);
      splitStaticLayersEntity_ = dragEntity;
      splitStaticLayersViewport_ = currentCanvasSize;
    }
  } else {
    backgroundBitmap_ = RendererBitmap();
    foregroundBitmap_ = RendererBitmap();
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

  // Slow-frame diagnostic. When renderFrame exceeds the reporting
  // threshold (1s), dump the state summary collected above to stderr so
  // the editor user (or a test harness running live) can reconstruct a
  // faithful repro. Kept behind a wall-clock check so the print cost
  // doesn't bleed into fast frames.
  const auto slowFrameElapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - slowFrameStart).count();
  constexpr double kSlowFrameThresholdMs = 1000.0;
  if (slowFrameElapsed >= kSlowFrameThresholdMs) {
    const Vector2i canvasSize = document_->canvasSize();
    std::fprintf(stderr,
                 "[CompositorSlowFrame] renderFrame %.1f ms | canvas=%dx%d | layers=%zu | "
                 "activeHints=%zu | segments=%zu (dirtyAtStart=%zu) | "
                 "rootDirtyAtStart=%d needsFullRebuildAtStart=%d | rasterizeLayerCalls=%zu\n",
                 slowFrameElapsed, canvasSize.x, canvasSize.y, layers_.size(),
                 activeHints_.size(), staticSegments_.size(), slowFrameSegmentDirtyCountAtStart,
                 slowFrameRootDirtyAtStart ? 1 : 0,
                 slowFrameNeedsFullRebuildAtStart ? 1 : 0, slowFrameRasterLog.size());
    for (const auto& reason : slowFrameRasterLog) {
      std::fprintf(stderr,
                   "[CompositorSlowFrame]   rasterizeLayer idx=%zu entity=%u dirty=%d "
                   "validBitmap=%d rootDirtyOnly=%d\n",
                   reason.layerIndex, static_cast<unsigned>(reason.entity),
                   reason.isDirty ? 1 : 0, reason.hasValidBitmap ? 1 : 0,
                   reason.rootDirtyTrigger ? 1 : 0);
    }
  }
}

size_t CompositorController::layerCount() const {
  return layers_.size();
}

size_t CompositorController::totalBitmapMemory() const {
  size_t total = backgroundBitmap_.pixels.size() + foregroundBitmap_.pixels.size();
  for (const auto& segment : staticSegments_) {
    total += segment.pixels.size();
  }
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
  ZoneScopedN("Compositor::rasterizeLayer");
  auto offscreen = renderer_->createOffscreenInstance();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);

  Registry& registry = document_->registry();

  RendererDriver driver(*offscreen);
  driver.drawEntityRange(registry, layer.firstEntity(), layer.lastEntity(), viewport,
                         Transform2d());

  // Stamp the bitmap with the entity's current absolute transform so the
  // fast path in `renderFrame` can later tell whether a DOM transform
  // mutation is a pure translation (reuse bitmap via `compositionTransform_`
  // delta) or a shape-changing transform (force re-rasterize). A missing
  // `RenderingInstanceComponent` means the entity was promoted before
  // the tree was prepared — treat it as transform identity, which makes
  // the subsequent fast-path delta equal to the new transform, which is
  // correct for the "bitmap was drawn at origin" case.
  Transform2d worldFromEntity;
  if (registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
    worldFromEntity =
        registry.get<components::RenderingInstanceComponent>(layer.entity()).worldFromEntityTransform;
  }
  layer.setBitmap(offscreen->takeSnapshot(), worldFromEntity);
  // Don't reset `compositionTransform_` here — a caller may have set it
  // explicitly (tests, editor drag hand-off paths) and expect that
  // additional offset to apply on top of the freshly-rasterized bitmap.
  // The fast path in `renderFrame` is what updates
  // `compositionTransform_` for DOM-driven deltas; rasterization itself
  // just refreshes the bitmap's content and the stamped
  // `bitmapEntityFromWorldTransform`.
}

void CompositorController::rasterizeDirtyStaticSegments(const RenderViewport& viewport) {
  ZoneScopedN("Compositor::rasterizeDirtyStaticSegmentsImpl");
  Registry& registry = document_->registry();
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

  // Snapshot paint order once per frame. Each segment is a slice of this
  // list — no per-segment document traversal needed.
  //
  // This replaces the old "hide every promoted layer + hide entities
  // outside this segment + `driver.draw(document)`" dance, which cost
  // 9× full-document walks AND 9× `prepareDocumentForRendering` per
  // click-drag frame on `donner_splash.svg`. By slicing the paint-order
  // list directly and calling `RendererDriver::drawEntityRange` on
  // just the segment's range, we avoid the per-segment prepare, skip
  // every registry-visibility mutation, and touch only entities that
  // actually draw into this segment. Side benefit: no shared mutation
  // state, so future parallelization is a drop-in (each segment gets
  // its own offscreen renderer and its own slice — no cross-thread
  // contention).
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
    ZoneScopedN("Compositor::rasterizeSegment");

    // Segment `i` spans paint order strictly between layer i-1 and
    // layer i (exclusive on both ends). Edge cases: segment 0 starts
    // at the document's first paint-ordered entity; segment N ends at
    // the last. If the computed range is empty (two layers back-to-
    // back in paint order), produce an empty canvas-sized bitmap.
    const size_t startIdx =
        (i == 0) ? 0u : (layerRanges[i - 1].lastIdx + 1u);
    const size_t endIdx =
        (i == layerCount)
            ? (paintOrder.empty() ? 0u : paintOrder.size() - 1u)
            : (layerRanges[i].firstIdx == 0u ? 0u : layerRanges[i].firstIdx - 1u);

    const bool segmentIsEmpty =
        paintOrder.empty() || startIdx > endIdx || startIdx >= paintOrder.size();

    if (segmentIsEmpty) {
      // No entities in this segment's paint-order range. Use a 1×1
      // transparent placeholder so the resync's `.empty()` test
      // distinguishes "not yet rasterized" from "rasterized with no
      // content". A `RendererBitmap()` default-construct would leave
      // `.empty()` true, causing the slot to be re-rasterized every
      // frame and cascading into a `recomposeSplitBitmaps` on every
      // drag frame (1.5 ms → 500 ms regression). Saves the canvas-
      // sized allocation the old code paid for empty segments.
      ZoneScopedN("Compositor::segment::emptyBitmap");
      RendererBitmap placeholder;
      placeholder.dimensions = Vector2i(1, 1);
      placeholder.rowBytes = 4u;
      placeholder.pixels.assign(4u, 0u);
      placeholder.alphaType = AlphaType::Premultiplied;
      staticSegments_[i] = std::move(placeholder);
      staticSegmentOffsets_[i] = Vector2d::Zero();
    } else {
      // Try the tight-bound path: ask the renderer driver what the
      // canvas-space bounds of the entity range will be BEFORE any
      // pixel allocation or rasterize. If it can give us a bounds
      // that's meaningfully smaller than the full canvas, size the
      // offscreen to that and shift all draws via the base transform.
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
        RendererDriver boundsDriver(*renderer_);
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
        offscreen = renderer_->createOffscreenInstance();
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
        staticSegments_[i] = offscreen->takeSnapshot();
        staticSegmentOffsets_[i] = tightBoundsSnapped.topLeft;
      } else {
        ZoneScopedN("Compositor::segment::drawEntityRange");
        RendererDriver driver(*offscreen);
        driver.drawEntityRange(registry, paintOrder[startIdx], paintOrder[endIdx], viewport,
                                Transform2d());
        staticSegments_[i] = offscreen->takeSnapshot();
        staticSegmentOffsets_[i] = Vector2d::Zero();
      }
    }
    staticSegmentDirty_[i] = false;
    // Bump the generation so the editor's GL texture cache sees this
    // slot's pixel content changed and re-uploads on next frame.
    if (i < staticSegmentGeneration_.size()) {
      staticSegmentGeneration_[i] = nextSegmentGeneration_++;
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
  std::vector<bool> newDirty(newCount, true);
  std::vector<uint64_t> newGeneration(newCount, 0);
  std::vector<Vector2d> newOffsets(newCount, Vector2d::Zero());

  if (!canvasChanged && !staticSegments_.empty()) {
    // Build a lookup from old boundary identity → (old slot index).
    // Small N (kMaxCompositorLayers = 32), so a linear scan beats a hash.
    for (size_t i = 0; i < newCount; ++i) {
      const auto& [left, right] = newBoundaries[i];
      for (size_t j = 0; j < staticSegmentBoundaries_.size(); ++j) {
        if (staticSegmentBoundaries_[j].first == left &&
            staticSegmentBoundaries_[j].second == right) {
          newSegments[i] = std::move(staticSegments_[j]);
          newDirty[i] = newSegments[i].empty();
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
      newGeneration[i] = nextSegmentGeneration_++;
    }
  }

  staticSegments_ = std::move(newSegments);
  staticSegmentDirty_ = std::move(newDirty);
  staticSegmentBoundaries_ = std::move(newBoundaries);
  staticSegmentGeneration_ = std::move(newGeneration);
  staticSegmentOffsets_ = std::move(newOffsets);
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

std::vector<CompositorTile> CompositorController::snapshotTilesForUpload() const {
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
  for (size_t i = 0; i < layers_.size(); ++i) {
    // Segment slot before layer[i]. Tile id = (left-neighbor entity,
    // right-neighbor entity).
    const auto& boundary = (i < staticSegmentBoundaries_.size())
                               ? staticSegmentBoundaries_[i]
                               : std::pair<Entity, Entity>{entt::null, entt::null};
    const uint64_t segGen =
        (i < staticSegmentGeneration_.size()) ? staticSegmentGeneration_[i] : 0;
    const RendererBitmap* segBitmap =
        (i < staticSegments_.size() && !staticSegments_[i].empty()) ? &staticSegments_[i]
                                                                    : nullptr;
    tiles.push_back(CompositorTile{
        .tileId = SegmentTileId(boundary.first, boundary.second),
        .generation = segGen,
        .paintOrderIndex = paintIdx++,
        .bitmap = segBitmap,
        .layerEntity = entt::null,
        .compositionTransform = Transform2d(),
    });
    // Layer tile.
    const auto& layer = layers_[i];
    const RendererBitmap* layerBitmap = layer.hasValidBitmap() ? &layer.bitmap() : nullptr;
    tiles.push_back(CompositorTile{
        .tileId = kLayerTileBit |
                  static_cast<uint64_t>(entt::to_integral(layer.entity())),
        .generation = layer.generation(),
        .paintOrderIndex = paintIdx++,
        .bitmap = layerBitmap,
        .layerEntity = layer.entity(),
        .compositionTransform = layer.compositionTransform(),
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
      (tailIdx < staticSegments_.size() && !staticSegments_[tailIdx].empty())
          ? &staticSegments_[tailIdx]
          : nullptr;
  tiles.push_back(CompositorTile{
      .tileId = SegmentTileId(tailBoundary.first, tailBoundary.second),
      .generation = tailGen,
      .paintOrderIndex = paintIdx,
      .bitmap = tailBitmap,
      .layerEntity = entt::null,
      .compositionTransform = Transform2d(),
  });
  return tiles;
}

size_t CompositorController::findSegmentForEntity(Entity entity) const {
  Registry& registry = document_->registry();
  if (!registry.valid(entity) ||
      !registry.all_of<components::RenderingInstanceComponent>(entity)) {
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

void CompositorController::recomposeSplitBitmaps(const CompositorLayer& dragLayer,
                                                 const RenderViewport& viewport) {
  ZoneScopedN("Compositor::recomposeSplitBitmaps");
  if (staticSegments_.empty()) {
    return;
  }

  // Find the drag layer's index in the paint-order-sorted `layers_` vector.
  size_t dragIdx = layers_.size();
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i].entity() == dragLayer.entity()) {
      dragIdx = i;
      break;
    }
  }
  if (dragIdx == layers_.size()) {
    return;
  }

  auto composeRange = [&](size_t layerStart, size_t layerEndExclusive) {
    ZoneScopedN("Compositor::composeRange");
    std::unique_ptr<RendererInterface> offscreen;
    {
      ZoneScopedN("Compositor::composeRange::createOffscreen");
      offscreen = renderer_->createOffscreenInstance();
    }
    UTILS_RELEASE_ASSERT(offscreen != nullptr);

    {
      ZoneScopedN("Compositor::composeRange::beginFrame");
      offscreen->beginFrame(viewport);
    }
    {
      ZoneScopedN("Compositor::composeRange::drawLoop");
      // Range covers segments [layerStart .. layerEndExclusive] plus the
      // non-drag promoted layers [layerStart .. layerEndExclusive - 1]
      // interleaved in paint order.
      for (size_t i = layerStart; i <= layerEndExclusive; ++i) {
        const RendererBitmap& segment = staticSegments_[i];
        if (!segment.empty()) {
          ZoneScopedN("Compositor::composeRange::drawSegment");
          ImageResource image = BuildImageResource(segment);
          ImageParams params;
          params.targetRect = Box2d(Vector2d::Zero(),
                                    Vector2d(static_cast<double>(segment.dimensions.x),
                                             static_cast<double>(segment.dimensions.y)));
          // Tight-bounded segments live at a non-origin offset on the
          // canvas; full-canvas-fallback segments use `Zero()`. Same
          // API shape either way.
          const Vector2d segmentOffset =
              i < staticSegmentOffsets_.size() ? staticSegmentOffsets_[i] : Vector2d::Zero();
          offscreen->setTransform(Transform2d::Translate(segmentOffset));
          offscreen->drawImage(image, params);
        }
        if (i < layerEndExclusive) {
          const CompositorLayer& promoted = layers_[i];
          if (promoted.hasValidBitmap()) {
            ZoneScopedN("Compositor::composeRange::drawPromoted");
            const RendererBitmap& bitmap = promoted.bitmap();
            Transform2d composition = promoted.compositionTransform();
            if (composition.isTranslation()) {
              Vector2d t = composition.translation();
              composition = Transform2d::Translate(std::round(t.x), std::round(t.y));
            }
            ImageResource image = BuildImageResource(bitmap);
            ImageParams params;
            params.targetRect = Box2d(Vector2d::Zero(),
                                      Vector2d(static_cast<double>(bitmap.dimensions.x),
                                               static_cast<double>(bitmap.dimensions.y)));
            offscreen->setTransform(composition);
            offscreen->drawImage(image, params);
          }
        }
      }
    }
    {
      ZoneScopedN("Compositor::composeRange::endFrame");
      offscreen->endFrame();
    }
    RendererBitmap snapshot;
    {
      ZoneScopedN("Compositor::composeRange::takeSnapshot");
      snapshot = offscreen->takeSnapshot();
    }
    return snapshot;
  };

  {
    ZoneScopedN("Compositor::composeRange::background");
    backgroundBitmap_ = composeRange(0, dragIdx);
  }
  {
    ZoneScopedN("Compositor::composeRange::foreground");
    foregroundBitmap_ = composeRange(dragIdx + 1, layers_.size());
  }
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
  // When the editor has a cached `bg / drag / fg` triple and is composing
  // the drag overlay itself via GL, the main-renderer compose output is
  // uploaded as `flatTexture` but typically not drawn — `RenderPane
  // Presenter` uses the split triple during drag and only falls back to
  // `flatTexture` when the presenter's display-mode check flips (drag
  // end, drag-target swap mid-transition, or selection clear).
  // Re-running 3+ full-canvas drawImage calls into a texture nobody
  // reads burned ~110 ms/frame at 892×512 on Skia, so we skip the
  // compose when `skipMainComposeDuringSplit_` is on and the split
  // cache is populated.
  //
  // The `skipMainCompose` path also skips `beginFrame`/`endFrame`
  // entirely: `beginFrame` recreates the renderer's pixmap as a fully
  // transparent buffer, so calling it and then NOT drawing anything
  // would leave a transparent flat-texture snapshot — and when the
  // presenter falls back to flat (e.g., user clicks a different shape
  // and the new composited result hasn't landed yet), the user would
  // see a one-frame transparent flash of the whole canvas. Skipping
  // both begin and end preserves the last non-split render (the cold
  // document) as a valid flat fallback across the entire drag session.
  // The post-drag settle render runs with `skipMainComposeDuringSplit_`
  // effectively off (via the settling-render path in AsyncRenderer), so
  // the flat fallback is refreshed before any display-mode transition
  // that would read it back.
  //
  // The skip is ALSO gated on "an ActiveDrag is in flight": selection-
  // only prewarm renders (e.g., mouse-hovering a selected element before
  // any drag) must still produce a fresh flat bitmap — that's what the
  // editor displays while waiting for the first composited frame. If
  // we skipped compose on those too, the flat texture would stay empty
  // across the whole selection-hold window. Check `activeHints_` for a
  // kind-ActiveDrag entry, not just "split layers present" — a
  // Selection-only promote produces split layers too.
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
  // (flat-texture upload, unit tests) get valid pixels. After the
  // first full compose lands, subsequent drag frames can safely skip
  // because `frame_` retains the prior pixmap.
  const bool skipMainCompose = skipMainComposeDuringSplit_ && hasActiveDrag &&
                               hasSplitStaticLayers() && mainRendererHasCachedFrame_;
  if (skipMainCompose) {
    return;
  }

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
    const Entity dragEntity = activeHints_.begin()->first;
    // Background/foreground composites are always canvas-sized — they
    // bake in the tight-bound offsets of their constituent segments,
    // so the editor can upload them as-is.
    drawBitmap(backgroundBitmap_, Transform2d());
    if (const CompositorLayer* dragLayer = findLayer(dragEntity)) {
      drawLayer(*dragLayer);
    }
    drawBitmap(foregroundBitmap_, Transform2d());
  } else if (!staticSegments_.empty()) {
    // Segments carry their tight-bound offset in `staticSegmentOffsets_`;
    // `Translate(offset)` places each bitmap at its canvas-space home.
    for (size_t i = 0; i < layers_.size(); ++i) {
      const Vector2d segmentOffset =
          i < staticSegmentOffsets_.size() ? staticSegmentOffsets_[i] : Vector2d::Zero();
      drawBitmap(staticSegments_[i], Transform2d::Translate(segmentOffset));
      drawLayer(layers_[i]);
    }
    const Vector2d lastOffset = staticSegmentOffsets_.size() == staticSegments_.size()
                                    ? staticSegmentOffsets_.back()
                                    : Vector2d::Zero();
    drawBitmap(staticSegments_.back(), Transform2d::Translate(lastOffset));
  }

  renderer_->endFrame();
  // Record that the main renderer's framebuffer now holds a full
  // compose — future drag frames can safely skip `composeLayers` and
  // `takeSnapshot` will still return a valid flat fallback.
  mainRendererHasCachedFrame_ = true;
}

void CompositorController::cascadeTransformDirtyToDescendantSegments(
    const std::vector<Entity>& transformDirtyEntities) {
  Registry& registry = document_->registry();
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
    // rooted at this entity), its content lives in the layer's cached
    // bitmap — NOT in any segment. The layer's bitmap is either reused
    // via a compose-transform (fast path) or re-rasterized via
    // `rasterizeLayer` (slow path); the segment carries zero pixels
    // that belong to this entity. Marking `findSegmentForEntity(entity)`
    // dirty just forces an unnecessary canvas-sized re-rasterize of an
    // adjacent segment on every drag frame — on the splash that's the
    // tail segment (100+ paths), costing ~40 ms/frame. Skip it.
    const auto* selfAssignment =
        registry.try_get<ComputedLayerAssignmentComponent>(entity);
    const bool entityIsPromotedLayerRoot =
        selfAssignment != nullptr && selfAssignment->layerId != 0;
    if (!entityIsPromotedLayerRoot &&
        registry.all_of<components::RenderingInstanceComponent>(entity)) {
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
      const auto* assignment =
          registry.try_get<ComputedLayerAssignmentComponent>(descendant);
      if (assignment != nullptr && assignment->layerId != 0) {
        continue;  // sub-layer: handled by its own compositionTransform.
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
  Registry& registry = document_->registry();

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
      [&registry](const CompositorLayer& layer) {
        if (!registry.valid(layer.entity())) {
          return true;
        }
        const auto* assignment =
            registry.try_get<ComputedLayerAssignmentComponent>(layer.entity());
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

    // Do NOT set `rootDirty_ = true` here — a layer-set change is handled
    // surgically by `resyncSegmentsToLayerSet`, which preserves cached
    // bitmaps for segments whose boundary identity survived. Setting
    // `rootDirty_` would wipe every cache on every drag-target promote,
    // producing a multi-second freeze on the user's first click even
    // though most segments (bg, far-away filter neighbors) are still
    // valid.
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
  // path via `compositionTransform`, so interactivity is preserved.
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

void CompositorController::propagateFastPathTranslationToSubtree(Registry& registry,
                                                                 Entity root,
                                                                 const Transform2d& delta) {
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

    if (auto* instance =
            registry.try_get<components::RenderingInstanceComponent>(descendant)) {
      // `worldFromEntity` maps entity-local points to world space. Since
      // only the root's local transform changed — by a pure world-space
      // translation — every descendant's world transform pre-multiplies
      // by that same delta (root-from-descendant is unchanged, world-
      // from-root shifts by delta, so world-from-descendant shifts too).
      instance->worldFromEntityTransform = delta * instance->worldFromEntityTransform;
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
