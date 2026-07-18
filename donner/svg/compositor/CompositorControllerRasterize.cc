/// @file
/// CompositorController rasterization and composition: dirty layer/segment
/// rasterization, segment/layer-set reconciliation, per-frame composition of
/// cached tiles and immediate spans, and the tile snapshot handed to the
/// editor for GPU upload.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/compositor/CompositorControllerInternal.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg::compositor {

namespace {

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
  return (l << 32) | r;  // bit 63 stays 0 - doesn't collide with layer ids.
}

}  // namespace

void CompositorController::rasterizeLayer(CompositorLayer& layer, const RenderViewport& viewport,
                                          const Transform2d& surfaceFromCanvas) {
  ZoneScopedN("Compositor::rasterizeLayer");
  const auto rasterizeStart = std::chrono::steady_clock::now();
  const ImmediateLayerPlan previousImmediatePlan = layer.immediatePlan();
  const bool wasDynamicImmediate = previousImmediatePlan.immediate &&
                                   previousImmediatePlan.dynamicHeuristicImmediate &&
                                   !previousImmediatePlan.staticHeuristicImmediate;

  Registry& registry = document().registry();
  const LayerRasterGeometry geometry = ComputeLayerRasterGeometry(
      renderer(), registry, layer.firstEntity(), layer.lastEntity(), viewport, surfaceFromCanvas);
  ImmediateLayerPlan immediatePlan;
  immediatePlan.visible = geometry.boundsCanvas.has_value();
  if (geometry.boundsCanvas.has_value()) {
    immediatePlan.boundsCanvas = *geometry.boundsCanvas;
  }
  const StaticSpanCostEstimate cost =
      EstimateEntityRangeCost(registry, layer.firstEntity(), layer.lastEntity());
  const FallbackReason immediateBlockingFallbacks =
      layer.fallbackReasons() &
      (FallbackReason::BlendMode | FallbackReason::Filter | FallbackReason::ClipPath |
       FallbackReason::Mask | FallbackReason::Markers);
  immediatePlan.estimatedDrawOps = cost.drawOps;
  immediatePlan.estimatedPathVerbs = cost.pathVerbs;
  immediatePlan.estimatedUsesAreaCostlyPaint = cost.usesAreaCostlyPaint;
  immediatePlan.hasExpensiveEffect =
      cost.hasExpensiveEffect || immediateBlockingFallbacks != FallbackReason::None;
  if (immediatePlan.visible) {
    const StaticSpanPresentationCost presentationCost =
        EstimateStaticSpanPresentationCost(cost, immediatePlan.boundsCanvas);
    immediatePlan.estimatedRetainedBytes = presentationCost.retainedBytes;
    immediatePlan.estimatedRedrawCost = presentationCost.redrawCost;
    immediatePlan.estimatedCacheOverheadCost = presentationCost.cacheOverheadCost;
    immediatePlan.staticHeuristicImmediate =
        IsImmediateSafe(immediatePlan.visible, immediatePlan.hasExpensiveEffect,
                        immediatePlan.estimatedDrawOps) &&
        (IsCheapDirectGeometry(cost) ||
         presentationCost.redrawCost <= presentationCost.cacheOverheadCost);
  }

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
  // the tree was prepared - treat it as transform identity, which makes
  // the subsequent fast-path delta equal to the new transform, which is
  // correct for the "bitmap was drawn at origin" case.
  Transform2d surfaceFromEntity;
  if (registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
    surfaceFromEntity = registry.get<components::RenderingInstanceComponent>(layer.entity())
                            .worldFromEntityTransform *
                        surfaceFromCanvas;
  }
  if (offscreen->requiresTextureSnapshotPresentation()) {
    std::shared_ptr<const RendererTextureSnapshot> texture = offscreen->takeTextureSnapshot();
    UTILS_RELEASE_ASSERT_MSG(
        texture != nullptr,
        "Geode compositor layer rasterization did not produce a GPU texture. Refusing CPU "
        "readback/upload fallback in Geode presentation mode.");
    layer.setTextureSnapshot(std::move(texture), surfaceFromEntity);
  } else {
    layer.setBitmap(offscreen->takeSnapshot(), surfaceFromEntity);
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
  const double elapsedMs = static_cast<double>(elapsedUs) / 1000.0;
  layer.setLastRasterizeMs(elapsedMs);
  immediatePlan.measuredRasterizeMs = elapsedMs;
  // Deterministic geometry estimate drives the decision; measured time above is
  // telemetry only. See `EstimateStaticSpanRasterizeMs`.
  const double estimatedRasterizeMs = EstimateStaticSpanRasterizeMs(
      immediatePlan.estimatedDrawOps, immediatePlan.estimatedPathVerbs,
      immediatePlan.estimatedUsesAreaCostlyPaint,
      static_cast<double>(immediatePlan.estimatedRetainedBytes) / 4.0);
  immediatePlan.estimatedRasterizeMs = estimatedRasterizeMs;
  immediatePlan.immediateBudgetMs = ImmediateStaticSpanBudgetMs();
  const bool interactionLayer =
      activeHints_.contains(layer.entity()) || layer.entity() == splitStaticLayersEntity_;
  const bool directLayerCandidate = interactionLayer || immediatePlan.staticHeuristicImmediate;
  if (directLayerCandidate &&
      IsImmediateSafe(immediatePlan.visible, immediatePlan.hasExpensiveEffect,
                      immediatePlan.estimatedDrawOps)) {
    const double budgetChargeMs = ImmediateStaticSpanBudgetChargeMs(estimatedRasterizeMs);
    immediatePlan.immediateBudgetChargeMs = budgetChargeMs;
    if (immediatePlan.staticHeuristicImmediate) {
      immediatePlan.immediate = true;
    } else if (interactionLayer && estimatedRasterizeMs <= immediatePlan.immediateBudgetMs &&
               budgetChargeMs <= immediatePlan.immediateBudgetMs) {
      immediatePlan.immediate = true;
      immediatePlan.dynamicHeuristicImmediate = true;
    }
  }
  if (wasDynamicImmediate && !immediatePlan.immediate &&
      estimatedRasterizeMs > immediatePlan.immediateBudgetMs) {
    immediatePlan.demotedDynamicImmediate = true;
  }
  layer.setImmediatePlan(immediatePlan);
  // Don't reset `canvasFromBitmap_` here - a caller may have set it
  // explicitly (tests, editor drag hand-off paths) and expect that
  // additional offset to apply on top of the freshly-rasterized bitmap.
  // The fast path in `renderFrame` is what updates `canvasFromBitmap_`
  // for DOM-driven deltas; rasterization itself just refreshes the
  // bitmap's content and the stamped `bitmapEntityFromWorldTransform`.
}

void CompositorController::rasterizeDirtyStaticSegments(const RenderViewport& viewport,
                                                        const Transform2d& surfaceFromCanvas) {
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
  if (staticSpanPlans_.size() != layerCount + 1) {
    staticSpanPlans_.resize(layerCount + 1);
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
  // Linear scan is fine - layerCount ≤ kMaxCompositorLayers (32) and
  // paintOrder is typically ~100 elements. Layers may have their root
  // entity at `firstEntity` and a subtree extending to `lastEntity`
  // elsewhere in the paint order - `computeEntityRange` stashed the
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

  double immediateBudgetUsedMs = 0.0;
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

    // Snapshot the slot's prior content + offset so we can detect a
    // no-op re-rasterize below. `Immediate`-mode segments are marked
    // dirty every frame (they're cheap enough to redraw rather than
    // cache), but redrawing byte-identical pixels must NOT advance the
    // generation - the generation is the editor's GL-texture-cache
    // invalidation key, and bumping it on unchanged content forces a
    // pointless re-upload and breaks the
    // SelectionToActiveDragDoesNotAdvanceUnchangedTileGenerations
    // contract (a hint-kind re-promote re-rasterizes a stable static
    // segment whose pixels never moved).
    RendererBitmap previousSegmentBitmap = staticSegments_[i];
    const std::shared_ptr<const RendererTextureSnapshot> previousSegmentTexture =
        staticSegmentTextures_[i];
    const Vector2d previousSegmentOffset = staticSegmentOffsets_[i];

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

    StaticSpanPlan spanPlan;
    spanPlan.slotIndex = i;
    const StaticSpanPlan previousSpanPlan =
        i < staticSpanPlans_.size() ? staticSpanPlans_[i] : StaticSpanPlan{};
    const bool wasImmediate = previousSpanPlan.mode == StaticSpanMode::Immediate;
    const bool wasDynamicImmediate = wasImmediate && previousSpanPlan.dynamicHeuristicImmediate &&
                                     !previousSpanPlan.staticHeuristicImmediate;
    if (!segmentIsEmpty) {
      spanPlan.firstEntity = paintOrder[startIdx];
      spanPlan.lastEntity = paintOrder[endIdx];
      spanPlan.spanRangeLabel = SpanRangeLabel(registry, spanPlan.firstEntity, spanPlan.lastEntity);
    }

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
      // patterns, sub-documents - see its own contract in
      // `RendererDriver.h`). Callers treat `nullopt` as "fall back
      // to full-canvas"; never as "empty segment". See design doc
      // 0027-tight_bounded_segments.md for which cases are pending
      // precise handling.
      std::optional<Box2d> tightBoundsCanvas;
      if (config_.tightBoundedSegments) {
        ZoneScopedN("Compositor::segment::computeBounds");
        RendererDriver boundsDriver(renderer());
        tightBoundsCanvas = boundsDriver.computeEntityRangeBounds(
            registry, paintOrder[startIdx], paintOrder[endIdx], viewport, surfaceFromCanvas);
      }
      // The bounds-compute overhead per segment is ~O(entities) with
      // no pixel work - negligible vs any render. But tight-bound
      // also adds the crop-into-smaller-bitmap overhead at compose
      // time; if the tight rect covers most of the canvas anyway,
      // the allocation savings don't justify it. Fall back to
      // full-canvas above the coverage threshold.
      constexpr double kTightBoundsCoverageThreshold = 0.75;
      const double canvasArea = viewport.size.x * viewport.size.y;
      bool useTight = false;
      bool visibleInViewport = false;
      Box2d tightBoundsSnapped;
      if (tightBoundsCanvas.has_value() && canvasArea > 0.0) {
        // Snap to integer pixels + 1px padding so AA edges aren't
        // clipped by the crop box - `computeEntityRangeBounds`
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
          visibleInViewport = true;
          const double tightArea = tightBoundsSnapped.width() * tightBoundsSnapped.height();
          if (tightArea < canvasArea * kTightBoundsCoverageThreshold) {
            useTight = true;
          }
        }
      }

      const StaticSpanCostEstimate cost =
          EstimateStaticSpanCost(registry, paintOrder, startIdx, endIdx);
      spanPlan.estimatedDrawOps = cost.drawOps;
      spanPlan.estimatedPathVerbs = cost.pathVerbs;
      spanPlan.estimatedUsesAreaCostlyPaint = cost.usesAreaCostlyPaint;
      spanPlan.hasExpensiveEffect = cost.hasExpensiveEffect;
      spanPlan.visible = visibleInViewport;
      if (visibleInViewport) {
        spanPlan.boundsCanvas = tightBoundsSnapped;
        const StaticSpanPresentationCost presentationCost =
            EstimateStaticSpanPresentationCost(cost, tightBoundsSnapped);
        spanPlan.estimatedRetainedBytes = presentationCost.retainedBytes;
        spanPlan.estimatedRedrawCost = presentationCost.redrawCost;
        spanPlan.estimatedCacheOverheadCost = presentationCost.cacheOverheadCost;
        spanPlan.staticHeuristicImmediate =
            IsStaticSpanImmediateSafe(spanPlan) &&
            presentationCost.redrawCost <= presentationCost.cacheOverheadCost;
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
        driver.drawEntityRange(
            registry, paintOrder[startIdx], paintOrder[endIdx], tightViewport,
            surfaceFromCanvas * Transform2d::Translate(-tightBoundsSnapped.topLeft));
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
                               surfaceFromCanvas);
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
    const auto segmentRasterizeEnd = std::chrono::steady_clock::now();
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               segmentRasterizeEnd - segmentRasterizeStart)
                               .count();
    double elapsedMs = static_cast<double>(elapsedUs) / 1000.0;
    if (staticSpanRasterizeElapsedMsForTesting_.has_value()) {
      elapsedMs = *staticSpanRasterizeElapsedMsForTesting_;
    }
    spanPlan.measuredRasterizeMs = elapsedMs;
    // Drive the immediate-vs-cached choice from a deterministic geometry estimate
    // (not the measured `elapsedMs`) so it no longer flaps with machine load. The
    // real time stays recorded above for telemetry / recalibration.
    const double estimatedRasterizeMs =
        EstimateStaticSpanRasterizeMs(spanPlan.estimatedDrawOps, spanPlan.estimatedPathVerbs,
                                      spanPlan.estimatedUsesAreaCostlyPaint,
                                      static_cast<double>(spanPlan.estimatedRetainedBytes) / 4.0);
    spanPlan.estimatedRasterizeMs = estimatedRasterizeMs;
    spanPlan.immediateBudgetMs = ImmediateStaticSpanBudgetMs();
    if (IsStaticSpanImmediateSafe(spanPlan)) {
      const double budgetChargeMs = ImmediateStaticSpanBudgetChargeMs(estimatedRasterizeMs);
      spanPlan.immediateBudgetChargeMs = budgetChargeMs;
      if (spanPlan.staticHeuristicImmediate) {
        spanPlan.mode = StaticSpanMode::Immediate;
        immediateBudgetUsedMs += budgetChargeMs;
      } else if (config_.dynamicImmediateStaticSpans &&
                 estimatedRasterizeMs <= spanPlan.immediateBudgetMs &&
                 immediateBudgetUsedMs + budgetChargeMs <= spanPlan.immediateBudgetMs) {
        spanPlan.mode = StaticSpanMode::Immediate;
        spanPlan.dynamicHeuristicImmediate = true;
        immediateBudgetUsedMs += budgetChargeMs;
      }
    }
    if (wasDynamicImmediate && spanPlan.mode != StaticSpanMode::Immediate &&
        estimatedRasterizeMs > spanPlan.immediateBudgetMs) {
      spanPlan.demotedDynamicImmediate = true;
    }
    if (!segmentIsEmpty) {
      if (!spanPlan.demotedDynamicImmediate && IsBoundedMultiDrawStaticSpan(spanPlan)) {
        spanPlan.mode = StaticSpanMode::Immediate;
        spanPlan.staticHeuristicImmediate = true;
        spanPlan.immediateBudgetChargeMs = ImmediateStaticSpanBudgetChargeMs(estimatedRasterizeMs);
      }
      const bool chargeAsImmediate = wasImmediate || spanPlan.mode == StaticSpanMode::Immediate;
      if (chargeAsImmediate) {
        lastRenderFrameStats_.immediateRasterizeMs += elapsedMs;
        ++lastRenderFrameStats_.immediateTileCount;
      } else {
        lastRenderFrameStats_.cachedRasterizeMs += elapsedMs;
        ++lastRenderFrameStats_.cachedTileCount;
      }
    }
    staticSpanPlans_[i] = spanPlan;
    // Immediate spans were introduced to avoid retaining and re-uploading cheap CPU bitmaps.
    // A texture snapshot has neither cost: it is already resident on the presentation device and
    // remains valid until content is explicitly invalidated. Re-rendering it every frame would
    // create a different snapshot identity even when the pixels are unchanged, forcing a needless
    // generation bump and GPU cache replacement. Keep texture-backed spans until a document,
    // transform, viewport, or topology change marks the slot dirty again.
    staticSegmentDirty_[i] = spanPlan.mode == StaticSpanMode::Immediate &&
                             !renderer().requiresTextureSnapshotPresentation();
    // Bump the generation only when this re-rasterize actually produced
    // different content (or moved the segment on the canvas). An
    // `Immediate` segment redraws every frame; if its entities and the
    // surfaceFromCanvas transform are unchanged it produces byte-
    // identical pixels at the same offset, and the editor's GL texture
    // cache must keep reusing its existing upload. Only a genuine pixel/
    // offset change is a cache-invalidation event.
    if (i < staticSegmentGeneration_.size()) {
      const bool offsetUnchanged =
          (staticSegmentOffsets_[i] - previousSegmentOffset).lengthSquared() == 0.0;
      const bool contentUnchanged = offsetUnchanged &&
                                    staticSegmentTextures_[i] == previousSegmentTexture &&
                                    BitmapPixelsEqual(staticSegments_[i], previousSegmentBitmap);
      if (!contentUnchanged || staticSegmentGeneration_[i] == 0) {
        staticSegmentGeneration_[i] = nextTileGeneration_++;
      }
    }
    if (i < staticSegmentLastRasterizeMs_.size()) {
      staticSegmentLastRasterizeMs_[i] = elapsedMs;
    }
  }
}

bool CompositorController::resyncSegmentsToLayerSet(const Vector2i& currentCanvasSize,
                                                    const Transform2d& surfaceFromCanvas) {
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

  // Canvas resized - bitmap caches are sized to the old canvas and can't
  // be reused at a different resolution (would be scaled or leave
  // transparent gaps). Full invalidation.
  const bool canvasChanged = staticSegmentsCanvas_ != currentCanvasSize;
  const bool surfaceChanged =
      hasStaticSegmentsSurfaceFromCanvas_ &&
      !SameTransformNear(staticSegmentsSurfaceFromCanvas_, surfaceFromCanvas);

  std::vector<RendererBitmap> newSegments(newCount);
  std::vector<std::shared_ptr<const RendererTextureSnapshot>> newTextures(newCount);
  std::vector<bool> newDirty(newCount, true);
  std::vector<uint64_t> newGeneration(newCount, 0);
  std::vector<Vector2d> newOffsets(newCount, Vector2d::Zero());
  std::vector<double> newLastRasterizeMs(newCount, 0.0);
  std::vector<StaticSpanPlan> newPlans(newCount);

  if (!canvasChanged && !surfaceChanged && !staticSegments_.empty()) {
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
            // texture cache reuses the existing binding - no upload.
            newGeneration[i] = staticSegmentGeneration_[j];
          }
          if (j < staticSegmentOffsets_.size()) {
            // Preserve the tight-bound offset alongside the bitmap -
            // the two together describe where the segment lives on
            // the canvas. A preserved bitmap with its offset dropped
            // would blit at (0,0) and wreck the compose.
            newOffsets[i] = staticSegmentOffsets_[j];
          }
          if (j < staticSegmentLastRasterizeMs_.size()) {
            newLastRasterizeMs[i] = staticSegmentLastRasterizeMs_[j];
          }
          if (j < staticSpanPlans_.size()) {
            newPlans[i] = staticSpanPlans_[j];
            newPlans[i].slotIndex = i;
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
  staticSpanPlans_ = std::move(newPlans);
  staticSegmentsCanvas_ = currentCanvasSize;
  staticSegmentsSurfaceFromCanvas_ = surfaceFromCanvas;
  hasStaticSegmentsSurfaceFromCanvas_ = true;
  staticSegmentsLayerCount_ = layers_.size();

  // bg/fg cache is ONLY reusable if every constituent bitmap survived.
  // Any preserved-segment map-miss means at least one rasterize is
  // pending, so bg/fg needs recompositing regardless. Caller drops the
  // cache when we return true.
  return canvasChanged || surfaceChanged ||
         std::any_of(staticSegmentDirty_.begin(), staticSegmentDirty_.end(),
                     [](bool dirty) { return dirty; });
}

std::vector<CompositorTile> CompositorController::snapshotTilesForUpload(
    CompositorTileBitmapPayload payload) const {
  // Interleaved tile list: segment[0], layer[0], segment[1], layer[1],
  // ..., layer[N-1], segment[N]. Matches the paint-order composition
  // in `composeLayers` so the editor can blit tiles by iterating the
  // vector in order.
  //
  // Segment tile ids encode the boundary pair (left-layer entity, right
  // -layer entity) so they're STABLE ACROSS INDEX SHIFTS - when a drag
  // target splits segment K into two, the segments at positions K-1
  // and K+1 in the new set (which are preserved content from old K-1
  // and K+1) keep the same tileId. The editor's GL texture cache keyed
  // on `tileId` recognizes the preservation and skips re-upload even
  // though the index position of the texture in the new interleaved
  // list shifted by one.
  //
  // Layer tile ids are `(1ull << 63) | entityId` - bit 63 is the
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
  const auto segmentImmediateAt = [this](size_t idx) {
    return idx < staticSpanPlans_.size() && staticSpanPlans_[idx].mode == StaticSpanMode::Immediate;
  };
  const auto includePayload = [payload](bool hasPayload, bool isDragTarget, bool immediate) {
    if (!hasPayload) return false;
    switch (payload) {
      case CompositorTileBitmapPayload::All: return true;
      case CompositorTileBitmapPayload::DragTargetOnly: return isDragTarget;
      case CompositorTileBitmapPayload::ImmediateOnly: return immediate;
      case CompositorTileBitmapPayload::ImmediateAndDragTargetOnly:
        return immediate || isDragTarget;
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
      const bool immediate = segmentImmediateAt(i);
      const bool includeSegPayload =
          includePayload(/*hasPayload=*/true, /*isDragTarget=*/false, immediate);
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
          .immediate = immediate,
      });
    }
    // Layer tile.
    const auto& layer = layers_[i];
    const RendererBitmap* layerBitmap = layer.hasValidBitmap() ? &layer.bitmap() : nullptr;
    const std::shared_ptr<const RendererTextureSnapshot> layerTexture = layer.textureSnapshot();
    const bool isDragTarget =
        layer.entity() == splitStaticLayersEntity_ || isActiveDragTarget(layer.entity());
    const bool immediate = layer.isImmediate();
    const bool includeLayerPayload =
        includePayload(layerBitmap != nullptr || layerTexture != nullptr, isDragTarget, immediate);
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
        .immediate = immediate,
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
    const bool immediate = segmentImmediateAt(tailIdx);
    const bool includeTailPayload =
        includePayload(/*hasPayload=*/true, /*isDragTarget=*/false, immediate);
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
        .immediate = immediate,
    });
  }
  return tiles;
}

void CompositorController::composeLayers(const RenderViewport& viewport,
                                         const Transform2d& surfaceFromCanvas) {
  ZoneScopedN("Compositor::composeLayersImpl");

  // Split path: bg/fg already composite segments + non-drag promoted
  // layers internally (see `recomposeSplitBitmaps` / `N=1` fast path), so
  // the main-renderer compose collapses to 3 `drawImage` calls - one for
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
  // present" - a Selection-only promote produces split layers too.
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

  const auto drawImmediateSpan = [&](size_t segmentIndex) {
    if (segmentIndex >= staticSpanPlans_.size()) {
      return false;
    }
    const StaticSpanPlan& spanPlan = staticSpanPlans_[segmentIndex];
    if (spanPlan.mode != StaticSpanMode::Immediate || spanPlan.firstEntity == entt::null ||
        spanPlan.lastEntity == entt::null) {
      return false;
    }

    const auto directStart = std::chrono::steady_clock::now();
    RendererDriver driver(renderer());
    driver.drawEntityRangeIntoCurrentFrame(document().registry(), spanPlan.firstEntity,
                                           spanPlan.lastEntity, viewport, surfaceFromCanvas);
    const auto directEnd = std::chrono::steady_clock::now();
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(directEnd - directStart).count();
    lastRenderFrameStats_.immediateRasterizeMs += static_cast<double>(elapsedUs) / 1000.0;
    return true;
  };

  const auto drawPayload = [this](const RendererBitmap* bitmap,
                                  const std::shared_ptr<const RendererTextureSnapshot>& texture,
                                  const Transform2d& canvasFromPayload) {
    const Vector2i payloadDims =
        bitmap != nullptr && HasPublicTileBitmap(*bitmap)
            ? bitmap->dimensions
            : (texture != nullptr ? texture->dimensions() : Vector2i::Zero());
    if (payloadDims.x <= 0 || payloadDims.y <= 0) {
      return;
    }
    ImageParams params;
    params.targetRect = Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(payloadDims.x),
                                                         static_cast<double>(payloadDims.y)));
    // Reset the renderer's per-element paint state to defaults (opacity 1.0)
    // before blitting a composited tile. The renderer convention is that
    // `setPaint` is called before every draw; this tile blit isn't a normal
    // element draw, so without it the blit inherits whatever `paintOpacity_` the
    // previous draw left behind. When the prior compose step direct-renders an
    // immediate layer whose range ends inside an opacity group (e.g. the
    // splash `#Clouds_with_gradients` group, opacity 0.75), that group opacity
    // leaks into this blit and dims the (already fully-composited) tile -
    // the #633 cached-segment drag divergence (the same tile drawn immediate
    // sets its own paint, which is why caching it exposed the leak).
    renderer().setPaint(PaintParams{});
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
    if (bitmap != nullptr && HasPublicTileBitmap(*bitmap)) {
      // drawBitmap consumes the premultiplied raster directly. Routing this
      // through the unpremultiplied ImageResource contract cost two full
      // pixel-buffer conversions plus three allocations per layer/segment per
      // composed frame - the dominant compose cost on drag-heavy replays.
      renderer().drawBitmap(*bitmap, params);
    }
  };

  const auto drawLayer = [&](const CompositorLayer& layer) {
    if (ShouldDirectComposeLayer(layer)) {
      const auto directStart = std::chrono::steady_clock::now();
      RendererDriver driver(renderer());
      driver.drawEntityRangeIntoCurrentFrame(document().registry(), layer.firstEntity(),
                                             layer.lastEntity(), viewport, surfaceFromCanvas);
      const auto directEnd = std::chrono::steady_clock::now();
      const auto elapsedUs =
          std::chrono::duration_cast<std::chrono::microseconds>(directEnd - directStart).count();
      lastRenderFrameStats_.immediateRasterizeMs += static_cast<double>(elapsedUs) / 1000.0;
      return;
    }
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
    const RendererBitmap* bitmap = layer.hasValidBitmap() ? &layer.bitmap() : nullptr;
    drawPayload(bitmap, layer.textureSnapshot(), canvasFromBitmap);
  };

  const auto drawSegment = [&](size_t segmentIndex) {
    if (drawImmediateSpan(segmentIndex)) {
      return;
    }

    const Vector2d segmentOffset = segmentIndex < staticSegmentOffsets_.size()
                                       ? staticSegmentOffsets_[segmentIndex]
                                       : Vector2d::Zero();
    const std::shared_ptr<const RendererTextureSnapshot> segmentTexture =
        segmentIndex < staticSegmentTextures_.size() ? staticSegmentTextures_[segmentIndex]
                                                     : nullptr;
    drawPayload(&staticSegments_[segmentIndex], segmentTexture,
                Transform2d::Translate(segmentOffset));
  };

  // Design doc 0033 §M2C: compose static segments and promoted layers in
  // interleaved paint order. Active drag frames may skip this main-renderer
  // compose via the `skipMainCompose` gate above.
  if (!staticSegments_.empty()) {
    for (size_t i = 0; i < layers_.size(); ++i) {
      drawSegment(i);
      drawLayer(layers_[i]);
    }
    drawSegment(staticSegments_.size() - 1u);
  }

  renderer().endFrame();
  // Record that the main renderer's framebuffer now holds a full
  // compose - future drag frames can safely skip `composeLayers` and
  // `takeSnapshot` will still return a valid full-canvas snapshot.
  mainRendererHasCachedFrame_ = true;
}

}  // namespace donner::svg::compositor
