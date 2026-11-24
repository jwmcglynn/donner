/// @file
/// CompositorController rasterization and composition: dirty layer/segment
/// rasterization, segment/layer-set reconciliation, per-frame composition of
/// cached tiles and immediate spans, and composition for presentation.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/components/AttachedIdLookup.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
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

// Sets a layer payload from an offscreen drawn under `raster`, recording that raster with it.
// Returns false, keeping the previous payload, when the snapshot failed: a null texture snapshot,
// which leaves the offscreen's drawn target attached, or an empty bitmap from a failed readback or
// a refused target.
bool SetLayerPayloadFromOffscreen(CompositorLayer& layer, RendererInterface& offscreen,
                                  const Transform2d& surfaceFromEntity,
                                  const CompositorLayer::PayloadRaster& raster) {
  if (!offscreen.supportsTextureSnapshotCompositing()) {
    RendererBitmap bitmap = offscreen.takeSnapshot();
    if (bitmap.empty()) {
      return false;
    }
    layer.setBitmap(std::move(bitmap), surfaceFromEntity, raster);
    return true;
  }
  std::shared_ptr<const RendererTextureSnapshot> texture = offscreen.takeTextureSnapshot();
  if (texture == nullptr) {
    return false;
  }
  layer.setTextureSnapshot(std::move(texture), surfaceFromEntity, raster);
  return true;
}

// Sets a static-segment payload from a drawn offscreen. Returns false, keeping the previous
// payload, when the snapshot failed: a null texture snapshot, which leaves the offscreen's drawn
// target attached, or an empty bitmap from a failed readback or a refused target.
bool SetSegmentPayloadFromOffscreen(RendererBitmap& segment,
                                    std::shared_ptr<const RendererTextureSnapshot>& segmentTexture,
                                    RendererInterface& offscreen) {
  if (!offscreen.supportsTextureSnapshotCompositing()) {
    RendererBitmap bitmap = offscreen.takeSnapshot();
    if (bitmap.empty()) {
      return false;
    }
    segment = std::move(bitmap);
    segmentTexture.reset();
    return true;
  }
  std::shared_ptr<const RendererTextureSnapshot> texture = offscreen.takeTextureSnapshot();
  if (texture == nullptr) {
    return false;
  }
  segment = RendererBitmap{};
  segmentTexture = std::move(texture);
  return true;
}

// A payload stays valid when only the surface translation changed: the layer's compose offset
// carries the shift. A new canvas size or a new scale, rotation or skew leaves it at another
// raster.
bool SameRasterUpToTranslation(const CompositorLayer::PayloadRaster& raster,
                               const Vector2i& canvasSize, const Transform2d& surfaceFromCanvas) {
  if (raster.canvasSize != canvasSize) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!NearEquals(raster.surfaceFromCanvas.data[i], surfaceFromCanvas.data[i])) {
      return false;
    }
  }
  return true;
}

// Size of the payload a compose step will draw: the public bitmap when present, else the
// texture, else zero.
Vector2i ComposePayloadDimensions(const RendererBitmap* bitmap,
                                  const std::shared_ptr<const RendererTextureSnapshot>& texture) {
  if (bitmap != nullptr && HasPublicTileBitmap(*bitmap)) {
    return bitmap->dimensions;
  }
  return texture != nullptr ? texture->dimensions() : Vector2i::Zero();
}

struct DamagePatchRect {
  int left;
  int top;
  int right;
  int bottom;
};

std::optional<DamagePatchRect> ComputeDamagePatchRect(const Box2d& damageBoundsCanvas,
                                                      const Vector2d& ownerOffset,
                                                      const Vector2i& ownerSize) {
  constexpr double kDamageHaloPx = 3.0;
  const Vector2d localMin = damageBoundsCanvas.topLeft - ownerOffset;
  const Vector2d localMax = damageBoundsCanvas.bottomRight - ownerOffset;
  if (!std::isfinite(localMin.x) || !std::isfinite(localMin.y) || !std::isfinite(localMax.x) ||
      !std::isfinite(localMax.y)) {
    return std::nullopt;
  }
  const auto edge = [](double value, int maximum) {
    return static_cast<int>(std::clamp(value, 0.0, static_cast<double>(maximum)));
  };
  const DamagePatchRect rect{edge(std::floor(localMin.x - kDamageHaloPx), ownerSize.x),
                             edge(std::floor(localMin.y - kDamageHaloPx), ownerSize.y),
                             edge(std::ceil(localMax.x + kDamageHaloPx), ownerSize.x),
                             edge(std::ceil(localMax.y + kDamageHaloPx), ownerSize.y)};
  if (rect.left >= rect.right || rect.top >= rect.bottom ||
      static_cast<std::int64_t>(rect.right - rect.left) * (rect.bottom - rect.top) * 3 >=
          static_cast<std::int64_t>(ownerSize.x) * ownerSize.y) {
    return std::nullopt;
  }
  return rect;
}

bool ComposeDamagePatch(RendererInterface& offscreen, const RendererTextureSnapshot& previous,
                        const RendererTextureSnapshot& patch, const RenderViewport& viewport,
                        const Vector2i& ownerSize, const DamagePatchRect& rect) {
  offscreen.beginFrame(viewport);
  offscreen.setPaint(PaintParams{});
  offscreen.setTransform(Transform2d());
  const Box2d fullRect(Vector2d::Zero(), Vector2d(ownerSize.x, ownerSize.y));
  const auto copyOldOutside = [&](const Box2d& clipRect) {
    if (clipRect.width() <= 0.0 || clipRect.height() <= 0.0) {
      return true;
    }
    ResolvedClip clip;
    clip.clipRect = clipRect;
    offscreen.pushClip(clip);
    const bool drawn = offscreen.drawTextureSnapshot(previous, fullRect, 1.0, true);
    offscreen.popClip();
    return drawn;
  };
  bool composed = true;
  composed &= copyOldOutside(Box2d(Vector2d::Zero(), Vector2d(ownerSize.x, rect.top)));
  composed &= copyOldOutside(Box2d(Vector2d(0, rect.bottom), Vector2d(ownerSize.x, ownerSize.y)));
  composed &= copyOldOutside(Box2d(Vector2d(0, rect.top), Vector2d(rect.left, rect.bottom)));
  composed &=
      copyOldOutside(Box2d(Vector2d(rect.right, rect.top), Vector2d(ownerSize.x, rect.bottom)));
  const Box2d patchRect(Vector2d(rect.left, rect.top), Vector2d(rect.right, rect.bottom));
  ResolvedClip clip;
  clip.clipRect = patchRect;
  offscreen.pushClip(clip);
  composed &= offscreen.drawTextureSnapshot(patch, patchRect, 1.0, true);
  offscreen.popClip();
  offscreen.endFrame();
  return composed;
}

void ConfigureImmediatePresentation(ImmediateLayerPlan& plan, bool wasDynamicImmediate,
                                    bool directInteractionLayer) {
  const double estimatedRasterizeMs = EstimateStaticSpanRasterizeMs(
      plan.estimatedDrawOps, plan.estimatedPathVerbs, plan.estimatedUsesAreaCostlyPaint,
      static_cast<double>(plan.estimatedRetainedBytes) / 4.0);
  plan.estimatedRasterizeMs = estimatedRasterizeMs;
  plan.immediateBudgetMs = ImmediateStaticSpanBudgetMs();
  if ((directInteractionLayer || plan.staticHeuristicImmediate) &&
      IsImmediateSafe(plan.visible, plan.hasExpensiveEffect, plan.estimatedDrawOps)) {
    const double chargeMs = ImmediateStaticSpanBudgetChargeMs(estimatedRasterizeMs);
    plan.immediateBudgetChargeMs = chargeMs;
    if (plan.staticHeuristicImmediate) {
      plan.immediate = true;
    } else if (directInteractionLayer && estimatedRasterizeMs <= plan.immediateBudgetMs &&
               chargeMs <= plan.immediateBudgetMs) {
      plan.immediate = true;
      plan.dynamicHeuristicImmediate = true;
    }
  }
  if (wasDynamicImmediate && !plan.immediate && estimatedRasterizeMs > plan.immediateBudgetMs) {
    plan.demotedDynamicImmediate = true;
  }
}

bool CanPatchLayerPayload(const RendererInterface& renderer, const CompositorLayer& layer) {
  return renderer.supportsTextureSnapshotCompositing() && !layer.isImmediate() &&
         layer.textureSnapshot() != nullptr && layer.canvasFromBitmap().isIdentity();
}

bool CanPatchLayerGeometry(const CompositorLayer& layer, const LayerRasterGeometry& geometry,
                           const Vector2i& ownerSize) {
  return !geometry.interactionBoundsRejected && geometry.tight &&
         ownerSize == layer.textureSnapshot()->dimensions() &&
         geometry.canvasOffset == layer.canvasOffset() && ownerSize.x > 0 && ownerSize.y > 0;
}

// Pieces drawn for a masked owner stand in for its tile only while the owner keeps the tile's size
// and placement; full-canvas and tight tiles both qualify.
bool OwnerTileGeometryUnchanged(const CompositorLayer& layer, const LayerRasterGeometry& geometry,
                                const Vector2i& ownerSize) {
  return !geometry.interactionBoundsRejected && ownerSize.x > 0 && ownerSize.y > 0 &&
         ownerSize == layer.textureSnapshot()->dimensions() &&
         geometry.canvasOffset == layer.canvasOffset();
}

// True when @p instance renders content instantiated after its host entity, past the end of its
// own range: a mask, markers, or pattern tiles, including a text span's own pattern tiles.
bool RendersAfterItself(const components::RenderingInstanceComponent& instance) {
  bool rendersContent = instance.mask.has_value() || instance.markerStart.has_value() ||
                        instance.markerMid.has_value() || instance.markerEnd.has_value();
  instance.forEachDrawnContent([&](const components::SubtreeInfo&) { rendersContent = true; });
  return rendersContent;
}

using EntityRun = std::pair<Entity, Entity>;

/// Runs of a masked owner's paint order before and after a target's subtree.
struct OwnerRunSplit {
  std::vector<EntityRun> below;
  std::vector<EntityRun> above;
};

/// Splits a masked owner's paint order around a target's subtree, one entity at a time. The owner
/// entity and its mask content are skipped: the per-frame mask pass replays them.
class OwnerRunSplitter {
public:
  OwnerRunSplitter(Registry& registry, Entity ownerEntity, EntityRun mask, EntityRun target)
      : registry_(registry), ownerEntity_(ownerEntity), mask_(mask), target_(target) {}

  /// Adds the next entity in paint order; false when the owner cannot be split around the target.
  bool add(Entity entity) {
    inMask_ = inMask_ || entity == mask_.first;
    const bool skipped = entity == ownerEntity_ || inMask_;
    if (entity == mask_.second) {
      inMask_ = false;
    }
    if (skipped) {
      closeRun();
      return !inTarget_;
    }
    if (entity == target_.first) {
      closeRun();
      inTarget_ = true;
    }
    if (inTarget_) {
      return addTargetEntity(entity);
    }
    extendRun(entity);
    return true;
  }

  /// The split, or nullopt when the target's subtree was not seen whole.
  std::optional<OwnerRunSplit> finish() {
    closeRun();
    if (!targetComplete_) {
      return std::nullopt;
    }
    return std::move(split_);
  }

private:
  bool addTargetEntity(Entity entity) {
    // Content instantiated after the host lies past the end of the target's range, outside its
    // piece; keep such targets on the full render.
    const auto* instance = registry_.try_get<components::RenderingInstanceComponent>(entity);
    if (instance != nullptr && RendersAfterItself(*instance)) {
      return false;
    }
    if (entity == target_.second) {
      inTarget_ = false;
      targetComplete_ = true;
      runs_ = &split_.above;
    }
    return true;
  }

  void extendRun(Entity entity) {
    if (openRun_.has_value()) {
      openRun_->second = entity;
    } else {
      openRun_ = EntityRun{entity, entity};
    }
  }

  void closeRun() {
    if (openRun_.has_value()) {
      runs_->push_back(*openRun_);
      openRun_.reset();
    }
  }

  Registry& registry_;
  Entity ownerEntity_;
  EntityRun mask_;
  EntityRun target_;
  OwnerRunSplit split_;
  std::vector<EntityRun>* runs_ = &split_.below;
  std::optional<EntityRun> openRun_;
  bool inMask_ = false;
  bool inTarget_ = false;
  bool targetComplete_ = false;
};

// Splits @p owner's paint order into runs before and after the @p target subtree's paint range,
// or nullopt when the owner cannot be drawn in pieces around it.
std::optional<OwnerRunSplit> SplitOwnerAroundTarget(Registry& registry,
                                                    const CompositorLayer& owner,
                                                    EntityRun target) {
  const auto& ownerInstance = registry.get<components::RenderingInstanceComponent>(owner.entity());
  if (!ownerInstance.mask.has_value() || !ownerInstance.mask->subtreeInfo.has_value()) {
    return std::nullopt;
  }
  OwnerRunSplitter splitter(registry, owner.entity(),
                            EntityRun{ownerInstance.mask->subtreeInfo->firstRenderedEntity,
                                      ownerInstance.mask->subtreeInfo->lastRenderedEntity},
                            target);
  RenderingInstanceView view(registry);
  while (!view.done() && view.currentEntity() != owner.firstEntity()) {
    view.advance();
  }
  bool reachedLast = false;
  while (!view.done() && !reachedLast) {
    const Entity entity = view.currentEntity();
    reachedLast = entity == owner.lastEntity();
    view.advance();
    if (!splitter.add(entity)) {
      return std::nullopt;
    }
  }
  return splitter.finish();
}

// True when the target's unclipped extent, with the antialiasing halo the tile bounds carry, fits
// @p ownerTileBounds. The target piece is clipped to the owner tile, so content outside it when
// the pieces are drawn could be dragged into view.
bool TargetFitsOwnerTile(RendererInterface& renderer, Registry& registry, EntityRun target,
                         const RenderViewport& viewport, const Transform2d& surfaceFromCanvas,
                         const Box2d& ownerTileBounds) {
  RendererDriver boundsDriver(renderer);
  const std::optional<Box2d> targetBounds = boundsDriver.computeEntityRangeBounds(
      registry, target.first, target.second, viewport, surfaceFromCanvas,
      RendererDriver::EntityRangeBoundsOptions{.clipToCanvas = false, .allowMaskSuperset = true});
  if (!targetBounds.has_value()) {
    return false;
  }
  constexpr double kEdgePaddingPx = 2.0;
  return targetBounds->topLeft.x - kEdgePaddingPx >= ownerTileBounds.topLeft.x &&
         targetBounds->topLeft.y - kEdgePaddingPx >= ownerTileBounds.topLeft.y &&
         targetBounds->bottomRight.x + kEdgePaddingPx <= ownerTileBounds.bottomRight.x &&
         targetBounds->bottomRight.y + kEdgePaddingPx <= ownerTileBounds.bottomRight.y;
}

// True when @p offsetPx is a whole number of pixels, so a shifted draw copies texels exactly.
bool IsWholePixelOffset(const Vector2d& offsetPx) {
  return std::abs(offsetPx.x - std::round(offsetPx.x)) < 1e-9 &&
         std::abs(offsetPx.y - std::round(offsetPx.y)) < 1e-9;
}

/// Where a static span's tile lands on the canvas.
struct SpanTileBounds {
  /// The span's canvas bounds clipped to the canvas, snapped to whole pixels with an antialiasing
  /// halo; unset when the span cannot be bounded or has nothing on the canvas.
  std::optional<Box2d> snapped;
  /// Crop the tile to \ref snapped instead of allocating the whole canvas.
  bool useTight = false;
  /// The span is bounded and lies wholly outside the canvas.
  bool offCanvas = false;
};

SpanTileBounds ComputeSpanTileBounds(RendererInterface& renderer, Registry& registry, Entity first,
                                     Entity last, const RenderViewport& viewport,
                                     const Transform2d& surfaceFromCanvas) {
  RendererDriver boundsDriver(renderer);
  std::optional<Box2d> bounds =
      boundsDriver.computeEntityRangeBounds(registry, first, last, viewport, surfaceFromCanvas);
  if (!bounds.has_value()) {
    // Clipped bounds are also absent for a span wholly outside the canvas; only the unclipped
    // bounds tell that apart from a span that cannot be bounded.
    bounds = boundsDriver.computeEntityRangeBounds(
        registry, first, last, viewport, surfaceFromCanvas,
        RendererDriver::EntityRangeBoundsOptions{.clipToCanvas = false});
  }
  const double canvasArea = viewport.size.x * viewport.size.y;
  SpanTileBounds result;
  if (!bounds.has_value() || canvasArea <= 0.0) {
    return result;
  }
  // Bounds are mathematical, not pixel aligned: a 1 px halo keeps antialiased edges in the crop.
  constexpr double kEdgePaddingPx = 1.0;
  result.offCanvas = PaddedBoundsMissCanvas(*bounds, kEdgePaddingPx, viewport.size);
  const Vector2d padding(kEdgePaddingPx, kEdgePaddingPx);
  const Box2d padded(bounds->topLeft - padding, bounds->bottomRight + padding);
  const Vector2d snapTL(std::floor(std::max(0.0, padded.topLeft.x)),
                        std::floor(std::max(0.0, padded.topLeft.y)));
  const Vector2d snapBR(std::ceil(std::min(viewport.size.x, padded.bottomRight.x)),
                        std::ceil(std::min(viewport.size.y, padded.bottomRight.y)));
  if (snapBR.x <= snapTL.x || snapBR.y <= snapTL.y) {
    return result;
  }
  result.snapped = Box2d(snapTL, snapBR);
  // Cropping costs a little at compose time; a span covering most of the canvas is not worth it.
  constexpr double kTightBoundsCoverageThreshold = 0.75;
  result.useTight = result.snapped->width() * result.snapped->height() <
                    canvasArea * kTightBoundsCoverageThreshold;
  return result;
}

// The entity's current absolute transform on the surface, which a layer payload is stamped with so
// a later pure translation can reuse it. An entity without a rendering instance was promoted before
// the tree was prepared and counts as drawn at the origin.
Transform2d SurfaceFromLayerEntity(Registry& registry, Entity entity,
                                   const Transform2d& surfaceFromCanvas) {
  if (!registry.all_of<components::RenderingInstanceComponent>(entity)) {
    return Transform2d();
  }
  return registry.get<components::RenderingInstanceComponent>(entity).worldFromEntityTransform *
         surfaceFromCanvas;
}

}  // namespace

bool DrawsCopyOf(Registry& registry, Entity target) {
  for (const auto& [entity, shadow] : registry.view<components::ShadowEntityComponent>().each()) {
    if (shadow.lightEntity == target || IsDomDescendantOf(registry, shadow.lightEntity, target)) {
      return true;
    }
  }
  for (const auto& [entity, image] : registry.view<components::FEImageComponent>().each()) {
    const std::string_view href(image.href);
    if (href.size() < 2 || href[0] != '#') {
      continue;
    }
    const Entity referenced =
        components::FindAttachedEntityById(registry, RcString(href.substr(1)));
    if (referenced != entt::null &&
        (referenced == target || IsDomDescendantOf(registry, referenced, target) ||
         IsDomDescendantOf(registry, target, referenced))) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<RendererInterface> CompositorController::acquireOffscreen() {
  if (pooledOffscreen_ != nullptr) {
    ++lastRenderFrameStats_.offscreenRecycleCount;
    ++lastRenderFrameStats_.offscreenRecycleTotal;
    return std::move(pooledOffscreen_);
  }
  ++lastRenderFrameStats_.offscreenCreateCount;
  ++lastRenderFrameStats_.offscreenCreateTotal;
  return renderer().createOffscreenInstance();
}

void CompositorController::recycleOffscreen(std::unique_ptr<RendererInterface> offscreen) {
  pooledOffscreen_ = std::move(offscreen);
}

std::uint64_t CompositorController::noteTileAttempt() {
  const std::uint64_t reserved = renderer().resourceStats().surfaceBytes;
  if (!bytesBeforeFirstTile_.has_value()) {
    bytesBeforeFirstTile_ = reserved;
  }
  return reserved;
}

bool CompositorController::refusedTileCannotFitAlone(std::uint64_t slotStartBytes,
                                                     const RendererInterface& offscreen) const {
  if (!tileAllocatedThisFrame_) {
    return true;
  }
  // What the tile reserved before the refusal, plus the reservation that was refused, is the least
  // the tile needs. A frame gives it at most the limit less what the frame reserved before its
  // first tile (its target, in a frame that drew flat first), whatever other tiles take.
  const RendererResourceStats stats = offscreen.resourceStats();
  const std::uint64_t reservedByTile =
      stats.surfaceBytes > slotStartBytes ? stats.surfaceBytes - slotStartBytes : 0;
  const std::uint64_t unavoidable = bytesBeforeFirstTile_.value_or(0);
  const std::uint64_t available =
      stats.surfaceLimitBytes > unavoidable ? stats.surfaceLimitBytes - unavoidable : 0;
  return stats.surfaceRefusedRequestBytes > available ||
         reservedByTile > available - stats.surfaceRefusedRequestBytes;
}

bool CompositorController::discardFailedOffscreen(std::unique_ptr<RendererInterface> offscreen) {
  const bool budgetRejected = offscreen->resourceStats().surfaceBudgetRejected;
  ++lastRenderFrameStats_.textureAllocationFailureCount;
  ++lastRenderFrameStats_.textureAllocationFailureTotal;
  if (budgetRejected) {
    surfaceBudgetExhaustedThisFrame_ = true;
  }
  return budgetRejected;
}

bool CompositorController::skipRasterizeUnderBudget(CompositorLayer& layer,
                                                    const ImmediateLayerPlan& previousPlan,
                                                    const Vector2i& canvasSize) {
  // Direct compose presents a layer the budget rejected twice at this canvas size, so no
  // further allocation is attempted until the size changes.
  const bool presentedDirectly =
      previousPlan.budgetImmediate && previousPlan.budgetCanvasSize == canvasSize;
  if (!presentedDirectly && !surfaceBudgetExhaustedThisFrame_) {
    return false;
  }
  if (!presentedDirectly && !(layer.isImmediate() && layer.hasRenderablePayload())) {
    // A latched rejection would refuse this allocation too. The layer may have reached this
    // point only because the whole tree was dirty, which no later frame repeats, so mark it
    // dirty explicitly. A clean immediate layer keeps its payload so the drag fast path can
    // still reuse it.
    layer.markDirty();
  }
  layer.setLastRasterizeMs(0.0);
  return true;
}

void CompositorController::recordLayerAllocationFailure(
    CompositorLayer& layer, const ImmediateLayerPlan& previousPlan,
    const ImmediateLayerPlan& freshPlan, const Vector2i& canvasSize,
    std::unique_ptr<RendererInterface> offscreen, bool failedAlone) {
  if (!discardFailedOffscreen(std::move(offscreen)) || !failedAlone) {
    return;
  }
  // The layer was rejected as the frame's first tile, so it cannot fit on its own. Keep the
  // previous plan's presentation choice and refresh only the geometry and cost estimates; a
  // second strike at the same canvas size stops retrying and presents the layer directly so
  // its content stays visible.
  ImmediateLayerPlan plan = previousPlan;
  plan.visible = freshPlan.visible;
  plan.boundsCanvas = freshPlan.boundsCanvas;
  plan.estimatedDrawOps = freshPlan.estimatedDrawOps;
  plan.estimatedPathVerbs = freshPlan.estimatedPathVerbs;
  plan.estimatedUsesAreaCostlyPaint = freshPlan.estimatedUsesAreaCostlyPaint;
  plan.hasExpensiveEffect = freshPlan.hasExpensiveEffect;
  plan.estimatedRetainedBytes = freshPlan.estimatedRetainedBytes;
  plan.estimatedRedrawCost = freshPlan.estimatedRedrawCost;
  plan.estimatedCacheOverheadCost = freshPlan.estimatedCacheOverheadCost;
  plan.staticHeuristicImmediate = freshPlan.staticHeuristicImmediate;
  plan.budgetImmediate = previousPlan.budgetRejected && previousPlan.budgetCanvasSize == canvasSize;
  plan.immediate = previousPlan.immediate || plan.budgetImmediate;
  plan.budgetRejected = true;
  plan.budgetCanvasSize = canvasSize;
  if (plan.budgetImmediate) {
    // Nothing draws a directly presented layer's retained payload; drop it so it stops
    // holding the memory the budget ran out of.
    layer.releasePayload();
  }
  layer.setImmediatePlan(plan);
}

bool CompositorController::adoptBudgetImmediateSpan(size_t slotIndex,
                                                    const StaticSpanPlan& previousPlan,
                                                    const StaticSpanPlan& freshPlan,
                                                    const Vector2i& canvasSize) {
  if (!previousPlan.budgetImmediate || previousPlan.budgetCanvasSize != canvasSize) {
    return false;
  }
  // The budget rejected this span twice at this canvas size; immediate presentation keeps its
  // content visible without another allocation attempt until the size changes.
  StaticSpanPlan plan = previousPlan;
  plan.slotIndex = slotIndex;
  plan.firstEntity = freshPlan.firstEntity;
  plan.lastEntity = freshPlan.lastEntity;
  plan.spanRangeLabel = freshPlan.spanRangeLabel;
  plan.mode = StaticSpanMode::Immediate;
  staticSpanPlans_[slotIndex] = plan;
  staticSegmentDirty_[slotIndex] = false;
  return true;
}

void CompositorController::recordSegmentAllocationFailure(
    size_t slotIndex, const StaticSpanPlan& previousPlan, const StaticSpanPlan& freshPlan,
    const Vector2i& canvasSize, std::unique_ptr<RendererInterface> offscreen, bool failedAlone) {
  const bool budgetRejected = discardFailedOffscreen(std::move(offscreen));
  // The slot keeps this frame's span range whatever the cause, so the tile set reads as
  // incomplete until the slot holds a payload. The previous plan's mode and cost telemetry
  // survive a failure so a span that was already immediate stays so.
  StaticSpanPlan plan = previousPlan;
  plan.slotIndex = slotIndex;
  plan.firstEntity = freshPlan.firstEntity;
  plan.lastEntity = freshPlan.lastEntity;
  plan.spanRangeLabel = freshPlan.spanRangeLabel;
  plan.visible = freshPlan.visible;
  plan.boundsCanvas = freshPlan.boundsCanvas;
  plan.estimatedDrawOps = freshPlan.estimatedDrawOps;
  plan.estimatedPathVerbs = freshPlan.estimatedPathVerbs;
  plan.estimatedUsesAreaCostlyPaint = freshPlan.estimatedUsesAreaCostlyPaint;
  plan.hasExpensiveEffect = freshPlan.hasExpensiveEffect;
  if (budgetRejected && failedAlone) {
    // A rejection as the frame's first tile is one strike; a second at the same canvas size
    // switches the span to immediate presentation. A span rejected after other tiles of the
    // frame took the budget is retried next frame without a strike.
    plan.budgetImmediate =
        previousPlan.budgetRejected && previousPlan.budgetCanvasSize == canvasSize;
    plan.budgetRejected = true;
    plan.budgetCanvasSize = canvasSize;
    if (plan.budgetImmediate) {
      plan.mode = StaticSpanMode::Immediate;
      // Nothing draws an immediately presented span's retained payload; release it.
      staticSegments_[slotIndex] = RendererBitmap{};
      staticSegmentTextures_[slotIndex].reset();
    }
  }
  staticSpanPlans_[slotIndex] = plan;
}

bool CompositorController::rasterizeSegmentSlot(
    size_t slotIndex, std::unique_ptr<RendererInterface> offscreen, Entity firstEntity,
    Entity lastEntity, const RenderViewport& viewport, const Transform2d& surfaceFromCanvas,
    const StaticSpanPlan& previousPlan, const StaticSpanPlan& freshPlan,
    const Vector2i& canvasSize) {
  const std::uint64_t slotStartBytes = noteTileAttempt();
  {
    RendererDriver driver(*offscreen, false, nullptr, styleWork_);
    if (!driver.drawEntityRangeInterruptibly(document().registry(), firstEntity, lastEntity,
                                             viewport, surfaceFromCanvas,
                                             [this]() { return isCancelled(); })) {
      return false;
    }
  }
  if (!SetSegmentPayloadFromOffscreen(staticSegments_[slotIndex], staticSegmentTextures_[slotIndex],
                                      *offscreen)) {
    const bool failedAlone = refusedTileCannotFitAlone(slotStartBytes, *offscreen);
    recordSegmentAllocationFailure(slotIndex, previousPlan, freshPlan, canvasSize,
                                   std::move(offscreen), failedAlone);
    return false;
  }
  tileAllocatedThisFrame_ = true;
  staticSegmentStyleWorkIncomplete_[slotIndex] = styleWork_->rejected();
  recycleOffscreen(std::move(offscreen));
  return true;
}

void CompositorController::yieldBetweenTiles() {
  if (config_.yieldBetweenTiles) {
    config_.yieldBetweenTiles();
  }
}

void CompositorController::rasterizeLayer(CompositorLayer& layer, const RenderViewport& viewport,
                                          const Transform2d& surfaceFromCanvas) {
  ZoneScopedN("Compositor::rasterizeLayer");
  const auto rasterizeStart = std::chrono::steady_clock::now();
  const ImmediateLayerPlan previousImmediatePlan = layer.immediatePlan();
  const bool wasDynamicImmediate = previousImmediatePlan.immediate &&
                                   previousImmediatePlan.dynamicHeuristicImmediate &&
                                   !previousImmediatePlan.staticHeuristicImmediate;
  const Vector2i canvasSize = BitmapDimensionsForViewport(viewport);
  if (skipRasterizeUnderBudget(layer, previousImmediatePlan, canvasSize)) {
    return;
  }

  Registry& registry = document().registry();
  const bool interactionLayer = activeHints_.contains(layer.entity());
  const LayerRasterGeometry geometry =
      ComputeLayerRasterGeometry(renderer(), registry, layer.firstEntity(), layer.lastEntity(),
                                 viewport, surfaceFromCanvas, interactionLayer);
  if (geometry.interactionBoundsRejected) {
    // An oversized full-object tile must never be silently replaced with a viewport-clipped one:
    // the old complete presentation remains available while the interaction uses owning tiles.
    ++lastRenderFrameStats_.textureAllocationFailureCount;
    ++lastRenderFrameStats_.textureAllocationFailureTotal;
    surfaceBudgetExhaustedThisFrame_ = true;
    layer.markDirty();
    return;
  }
  if (geometry.offCanvas) {
    // A placeholder stands in for the tile of a layer with nothing on this canvas, which would
    // otherwise rasterize into a transparent canvas-sized texture.
    layer.setBitmap(TransparentPlaceholderBitmap(),
                    SurfaceFromLayerEntity(registry, layer.entity(), surfaceFromCanvas),
                    CompositorLayer::PayloadRaster{.canvasSize = canvasSize,
                                                   .surfaceFromCanvas = surfaceFromCanvas});
    layer.setGeneration(nextTileGeneration_++);
    layer.setCanvasOffset(Vector2d::Zero());
    layer.setLastRasterizeMs(0.0);
    layer.setImmediatePlan(ImmediateLayerPlan{});
    return;
  }
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

  const std::uint64_t slotStartBytes = noteTileAttempt();
  auto offscreen = acquireOffscreen();
  // The offscreen-support probe in `renderFrameImpl` / `warmFirstFrameCaches`
  // latches off before any rasterize runs when creation fails, so a null here
  // is unreachable in practice. Keep the assert to catch pool corruption.
  UTILS_RELEASE_ASSERT(offscreen != nullptr);

  {
    ZoneScopedN("Compositor::rasterizeLayer::drawEntityRange");
    RendererDriver driver(*offscreen, false, nullptr, styleWork_);
    if (!driver.drawEntityRangeInterruptibly(registry, layer.firstEntity(), layer.lastEntity(),
                                             geometry.viewport, geometry.surfaceFromCanvas,
                                             [this]() { return isCancelled(); })) {
      return;
    }
  }

  // Stamp the payload so the fast path in `renderFrame` can tell a pure translation, which reuses
  // it, from a shape-changing transform, which re-rasterizes.
  const Transform2d surfaceFromEntity =
      SurfaceFromLayerEntity(registry, layer.entity(), surfaceFromCanvas);
  const CompositorLayer::PayloadRaster payloadRaster{.canvasSize = canvasSize,
                                                     .surfaceFromCanvas = surfaceFromCanvas,
                                                     .styleWorkIncomplete = styleWork_->rejected()};
  if (!SetLayerPayloadFromOffscreen(layer, *offscreen, surfaceFromEntity, payloadRaster)) {
    const bool failedAlone = refusedTileCannotFitAlone(slotStartBytes, *offscreen);
    recordLayerAllocationFailure(layer, previousImmediatePlan, immediatePlan, canvasSize,
                                 std::move(offscreen), failedAlone);
    layer.markDirty();
    return;
  }
  tileAllocatedThisFrame_ = true;
  // The snapshot detached the offscreen's target; the instance is clean and
  // reusable. Cancellation paths above return without recycling, so a
  // half-drawn frame's state is destroyed rather than pooled.
  recycleOffscreen(std::move(offscreen));
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
  // Deterministic geometry estimate drives the decision; measured time is telemetry only.
  ConfigureImmediatePresentation(immediatePlan, wasDynamicImmediate,
                                 interactionLayer || layer.entity() == splitStaticLayersEntity_);
  layer.setImmediatePlan(immediatePlan);
  // Don't reset `canvasFromBitmap_` here - a caller may have set it
  // explicitly (tests, editor drag hand-off paths) and expect that
  // additional offset to apply on top of the freshly-rasterized bitmap.
  // The fast path in `renderFrame` is what updates `canvasFromBitmap_`
  // for DOM-driven deltas; rasterization itself just refreshes the
  // bitmap's content and the stamped `bitmapEntityFromWorldTransform`.
  yieldBetweenTiles();
}

bool CompositorController::rasterizeLayerDamage(CompositorLayer& layer,
                                                const RenderViewport& viewport,
                                                const Transform2d& surfaceFromCanvas,
                                                const Box2d& damageBoundsCanvas) {
  if (!CanPatchLayerPayload(renderer(), layer)) {
    return false;
  }
  Registry& registry = document().registry();
  const LayerRasterGeometry geometry = ComputeLayerRasterGeometry(
      renderer(), registry, layer.firstEntity(), layer.lastEntity(), viewport, surfaceFromCanvas);
  const Vector2i ownerSize = BitmapDimensionsForViewport(geometry.viewport);
  if (!CanPatchLayerGeometry(layer, geometry, ownerSize)) {
    return false;
  }

  // Crop to the union of the moved child's old and new geometry, with an AA/stroke halo. The
  // region is in the owner's bitmap pixel space; the artboard/static tiles never grow for it.
  const std::optional<DamagePatchRect> rect =
      ComputeDamagePatchRect(damageBoundsCanvas, layer.canvasOffset(), ownerSize);
  if (!rect.has_value()) {
    return false;
  }

  const auto started = std::chrono::steady_clock::now();
  const std::shared_ptr<const RendererTextureSnapshot> previous = layer.textureSnapshot();
  std::unique_ptr<RendererInterface> offscreen = acquireOffscreen();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);
  const RendererResourceStats beforePatchStats = offscreen->resourceStats();
  RenderViewport patchViewport = geometry.viewport;
  patchViewport.size = Vector2d(rect->right - rect->left, rect->bottom - rect->top);
  const Transform2d patchSurface =
      geometry.surfaceFromCanvas * Transform2d::Translate(Vector2d(-rect->left, -rect->top));
  RendererDriver patchDriver(*offscreen, false, nullptr, styleWork_);
  patchDriver.setConservativeEarlyLeafCulling(true);
  if (!patchDriver.drawEntityRangeInterruptibly(registry, layer.firstEntity(), layer.lastEntity(),
                                                patchViewport, patchSurface,
                                                [this]() { return isCancelled(); })) {
    return false;
  }
  const RendererResourceStats patchStats = offscreen->resourceStats();
  std::shared_ptr<const RendererTextureSnapshot> patch = offscreen->takeTextureSnapshot();
  if (patch == nullptr) {
    discardFailedOffscreen(std::move(offscreen));
    return false;
  }

  if (!ComposeDamagePatch(*offscreen, *previous, *patch, geometry.viewport, ownerSize, *rect)) {
    discardFailedOffscreen(std::move(offscreen));
    return false;
  }

  Transform2d surfaceFromEntity;
  if (registry.all_of<components::RenderingInstanceComponent>(layer.entity())) {
    surfaceFromEntity = registry.get<components::RenderingInstanceComponent>(layer.entity())
                            .worldFromEntityTransform *
                        surfaceFromCanvas;
  }
  const CompositorLayer::PayloadRaster raster{
      .canvasSize = BitmapDimensionsForViewport(viewport),
      .surfaceFromCanvas = surfaceFromCanvas,
      .styleWorkIncomplete = styleWork_->rejected() || layer.styleWorkIncomplete(),
  };
  if (!SetLayerPayloadFromOffscreen(layer, *offscreen, surfaceFromEntity, raster)) {
    discardFailedOffscreen(std::move(offscreen));
    return false;
  }
  tileAllocatedThisFrame_ = true;
  layer.setGeneration(nextTileGeneration_++);
  layer.setCanvasOffset(geometry.canvasOffset);
  layer.setLastRasterizeMs(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
          .count());
  ++lastRenderFrameStats_.damagePatchTileCount;
  lastRenderFrameStats_.damagePatchGeometryDraws +=
      patchStats.geometryDraws >= beforePatchStats.geometryDraws
          ? patchStats.geometryDraws - beforePatchStats.geometryDraws
          : patchStats.geometryDraws;
  recycleOffscreen(std::move(offscreen));
  yieldBetweenTiles();
  return true;
}

bool CompositorController::buildMaskedChildDragPieces(CompositorLayer& owner, Entity target,
                                                      const RenderViewport& viewport,
                                                      const Transform2d& surfaceFromCanvas) {
  maskedChildDragPieces_.reset();
  if (!CanPatchLayerPayload(renderer(), owner)) {
    return false;
  }
  Registry& registry = document().registry();
  const LayerRasterGeometry geometry = ComputeLayerRasterGeometry(
      renderer(), registry, owner.firstEntity(), owner.lastEntity(), viewport, surfaceFromCanvas);
  const Vector2i ownerSize = BitmapDimensionsForViewport(geometry.viewport);
  if (!OwnerTileGeometryUnchanged(owner, geometry, ownerSize) || DrawsCopyOf(registry, target)) {
    return false;
  }
  const EntityRun targetRange = computeEntityRange(registry, target);
  const std::optional<OwnerRunSplit> split = SplitOwnerAroundTarget(registry, owner, targetRange);
  const Box2d ownerTileBounds(geometry.canvasOffset,
                              geometry.canvasOffset + Vector2d(ownerSize.x, ownerSize.y));
  if (!split.has_value() || !TargetFitsOwnerTile(renderer(), registry, targetRange, viewport,
                                                 surfaceFromCanvas, ownerTileBounds)) {
    return false;
  }

  // Each piece is drawn without the owner's mask into a tile of the owner's size and placement.
  std::optional<std::shared_ptr<const RendererTextureSnapshot>> below = drawMaskedChildDragPiece(
      registry, split->below, geometry.viewport, geometry.surfaceFromCanvas);
  if (!below.has_value()) {
    return false;
  }
  const std::array<EntityRun, 1> targetRuns{targetRange};
  std::optional<std::shared_ptr<const RendererTextureSnapshot>> targetPiece =
      drawMaskedChildDragPiece(registry, targetRuns, geometry.viewport, geometry.surfaceFromCanvas);
  if (!targetPiece.has_value() || *targetPiece == nullptr) {
    return false;
  }
  std::optional<std::shared_ptr<const RendererTextureSnapshot>> above = drawMaskedChildDragPiece(
      registry, split->above, geometry.viewport, geometry.surfaceFromCanvas);
  if (!above.has_value()) {
    return false;
  }

  maskedChildDragPieces_ = MaskedChildDragPieces{
      .owner = owner.entity(),
      .target = target,
      .below = std::move(*below),
      .targetPiece = std::move(*targetPiece),
      .above = std::move(*above),
      .ownerBitmapFromCanvas = geometry.surfaceFromCanvas,
      .ownerBitmapFromTargetAtStamp =
          registry.get<components::RenderingInstanceComponent>(target).worldFromEntityTransform *
          geometry.surfaceFromCanvas,
      .ownerSize = ownerSize,
      .canvasSize = BitmapDimensionsForViewport(viewport),
      .surfaceFromCanvas = surfaceFromCanvas,
      .ownerGeneration = owner.generation(),
      .styleWorkIncomplete = styleWork_->rejected(),
  };
  return true;
}

std::optional<std::shared_ptr<const RendererTextureSnapshot>>
CompositorController::drawMaskedChildDragPiece(Registry& registry,
                                               std::span<const std::pair<Entity, Entity>> runs,
                                               const RenderViewport& tileViewport,
                                               const Transform2d& tileSurfaceFromCanvas) {
  if (runs.empty()) {
    return std::shared_ptr<const RendererTextureSnapshot>();
  }
  std::unique_ptr<RendererInterface> offscreen = acquireOffscreen();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);
  offscreen->beginFrame(tileViewport);
  RendererDriver driver(*offscreen, false, nullptr, styleWork_);
  for (const auto& [first, last] : runs) {
    driver.drawEntityRangeIntoCurrentFrame(registry, first, last, tileViewport,
                                           tileSurfaceFromCanvas);
  }
  offscreen->endFrame();
  std::shared_ptr<const RendererTextureSnapshot> texture =
      isCancelled() ? nullptr : offscreen->takeTextureSnapshot();
  if (texture == nullptr) {
    discardFailedOffscreen(std::move(offscreen));
    return std::nullopt;
  }
  tileAllocatedThisFrame_ = true;
  recycleOffscreen(std::move(offscreen));
  return texture;
}

bool CompositorController::drawShiftedMaskedChildDragPieces(RendererInterface& renderer,
                                                            const MaskedChildDragPieces& pieces,
                                                            const Vector2d& targetOffsetPx) {
  renderer.setPaint(PaintParams{});
  const Box2d ownerRect(Vector2d::Zero(), Vector2d(pieces.ownerSize.x, pieces.ownerSize.y));
  const Box2d targetRect(ownerRect.topLeft + targetOffsetPx,
                         ownerRect.bottomRight + targetOffsetPx);
  bool composed = true;
  if (pieces.below != nullptr) {
    composed &= renderer.drawTextureSnapshot(*pieces.below, ownerRect, 1.0, true);
  }
  // A whole-pixel shift copies texels exactly; a fractional shift filters them.
  composed &= renderer.drawTextureSnapshot(*pieces.targetPiece, targetRect, 1.0,
                                           IsWholePixelOffset(targetOffsetPx));
  if (pieces.above != nullptr) {
    composed &= renderer.drawTextureSnapshot(*pieces.above, ownerRect, 1.0, true);
  }
  return composed;
}

bool CompositorController::composeMaskedOwnerFromPieces(CompositorLayer& owner,
                                                        const Vector2d& targetOffsetPx,
                                                        const RenderViewport& viewport,
                                                        const Transform2d& surfaceFromCanvas) {
  if (!maskedChildDragPieces_.has_value() || maskedChildDragPieces_->owner != owner.entity() ||
      !CanPatchLayerPayload(renderer(), owner)) {
    return false;
  }
  MaskedChildDragPieces& pieces = *maskedChildDragPieces_;
  Registry& registry = document().registry();
  // The moved child must still fit the owner's tile; a grown source bound needs the full raster.
  const LayerRasterGeometry geometry = ComputeLayerRasterGeometry(
      renderer(), registry, owner.firstEntity(), owner.lastEntity(), viewport, surfaceFromCanvas);
  if (BitmapDimensionsForViewport(geometry.viewport) != pieces.ownerSize ||
      !OwnerTileGeometryUnchanged(owner, geometry, pieces.ownerSize)) {
    return false;
  }

  const auto started = std::chrono::steady_clock::now();
  std::unique_ptr<RendererInterface> offscreen = acquireOffscreen();
  UTILS_RELEASE_ASSERT(offscreen != nullptr);
  offscreen->beginFrame(geometry.viewport);
  RendererDriver driver(*offscreen, false, nullptr, styleWork_);
  const bool drawn = driver.drawUnderEntityMaskIntoCurrentFrame(
      registry, owner.entity(), geometry.viewport, geometry.surfaceFromCanvas,
      [&] { return drawShiftedMaskedChildDragPieces(*offscreen, pieces, targetOffsetPx); });
  offscreen->endFrame();
  if (!drawn) {
    discardFailedOffscreen(std::move(offscreen));
    return false;
  }

  Transform2d surfaceFromEntity;
  if (registry.all_of<components::RenderingInstanceComponent>(owner.entity())) {
    surfaceFromEntity = registry.get<components::RenderingInstanceComponent>(owner.entity())
                            .worldFromEntityTransform *
                        surfaceFromCanvas;
  }
  const CompositorLayer::PayloadRaster raster{
      .canvasSize = BitmapDimensionsForViewport(viewport),
      .surfaceFromCanvas = surfaceFromCanvas,
      .styleWorkIncomplete = styleWork_->rejected() || pieces.styleWorkIncomplete,
  };
  if (!SetLayerPayloadFromOffscreen(owner, *offscreen, surfaceFromEntity, raster)) {
    discardFailedOffscreen(std::move(offscreen));
    return false;
  }
  tileAllocatedThisFrame_ = true;
  owner.setGeneration(nextTileGeneration_++);
  owner.setCanvasOffset(geometry.canvasOffset);
  owner.setLastRasterizeMs(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
          .count());
  pieces.ownerGeneration = owner.generation();
  pieces.composed = true;
  ++lastRenderFrameStats_.maskedChildComposeTileCount;
  recycleOffscreen(std::move(offscreen));
  return true;
}

namespace {

/// A promoted layer's first and last index in paint order. Both equal the paint order's size when
/// the layer's entities are not in it.
struct LayerPaintRange {
  size_t firstIdx = 0;  ///< Index of the layer's first entity.
  size_t lastIdx = 0;   ///< Index of the last entity that belongs to the layer.
};

/// Extends @p lastIdx over the mask, marker and pattern content drawn for the entities in
/// `[firstIdx, lastIdx]`. That content is instantiated after the element that draws it, so it can
/// follow the layer's last entity in paint order. It belongs to the layer: the next segment must
/// start after it, or it would draw that content as ordinary content.
size_t LastIndexWithDrawnContent(Registry& registry, const std::vector<Entity>& paintOrder,
                                 size_t firstIdx, size_t lastIdx) {
  int coveredDrawOrder =
      registry.get<components::RenderingInstanceComponent>(paintOrder[lastIdx]).drawOrder;
  for (size_t j = firstIdx; j < paintOrder.size(); ++j) {
    const auto& instance = registry.get<components::RenderingInstanceComponent>(paintOrder[j]);
    if (j > lastIdx && instance.drawOrder > coveredDrawOrder) {
      break;
    }
    coveredDrawOrder =
        std::max(coveredDrawOrder, RendererDriver::LastDrawOrderOfDrawnContent(registry, instance));
    lastIdx = std::max(lastIdx, j);
  }
  return lastIdx;
}

/// Finds the paint-order indices of a layer whose entity range is `[first, last]`, with the
/// content drawn for its elements included.
LayerPaintRange FindLayerPaintRange(Registry& registry, const std::vector<Entity>& paintOrder,
                                    Entity first, Entity last) {
  size_t firstIdx = paintOrder.size();
  size_t lastIdx = paintOrder.size();
  for (size_t j = 0; j < paintOrder.size(); ++j) {
    if (paintOrder[j] == first) {
      firstIdx = j;
    }
    if (paintOrder[j] == last) {
      lastIdx = j;
      break;
    }
  }
  if (firstIdx < paintOrder.size() && lastIdx < paintOrder.size()) {
    lastIdx = LastIndexWithDrawnContent(registry, paintOrder, firstIdx, lastIdx);
  }
  return {firstIdx, lastIdx};
}

}  // namespace

void CompositorController::holdEmptySegment(size_t slotIndex) {
  staticSegments_[slotIndex] = TransparentPlaceholderBitmap();
  staticSegmentStyleWorkIncomplete_[slotIndex] = false;
  staticSegmentTextures_[slotIndex].reset();
  staticSegmentOffsets_[slotIndex] = Vector2d::Zero();
}

void CompositorController::rasterizeDirtyStaticSegments(const RenderViewport& viewport,
                                                        const Transform2d& surfaceFromCanvas) {
  ZoneScopedN("Compositor::rasterizeDirtyStaticSegmentsImpl");
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
  staticSegmentStyleWorkIncomplete_.resize(layerCount + 1, false);
  if (staticSegmentTextures_.size() != layerCount + 1) {
    staticSegmentTextures_.resize(layerCount + 1);
  }
  if (staticSegmentLastRasterizeMs_.size() != layerCount + 1) {
    staticSegmentLastRasterizeMs_.resize(layerCount + 1, 0.0);
  }
  if (staticSpanPlans_.size() != layerCount + 1) {
    staticSpanPlans_.resize(layerCount + 1);
  }

  if (std::none_of(staticSegmentDirty_.begin(), staticSegmentDirty_.end(),
                   [](bool dirty) { return dirty; })) {
    return;
  }

  Registry& registry = document().registry();

  // Snapshot paint order once per frame. Each segment is a slice of this list,
  // rendered by entity range without mutating registry visibility.
  std::vector<Entity> paintOrder;
  {
    ZoneScopedN("Compositor::paintOrderSnapshot");
    ++fastPathCounters_.paintOrderSnapshots;
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
  std::vector<LayerPaintRange> layerRanges(layerCount);
  for (size_t i = 0; i < layerCount; ++i) {
    layerRanges[i] = FindLayerPaintRange(registry, paintOrder, layers_[i].firstEntity(),
                                         layers_[i].lastEntity());
  }

  double immediateBudgetUsedMs = 0.0;
  for (size_t i = 0; i <= layerCount; ++i) {
    if (!staticSegmentDirty_[i]) {
      continue;
    }
    // Bail between segment rasterizes. Leaves `staticSegmentDirty_`
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
      ZoneScopedN("Compositor::segment::emptyBitmap");
      holdEmptySegment(i);
    } else {
      // Try the tight-bound path before allocating pixels: a cropped offscreen draws shifted by the
      // crop origin. `ComputeSpanTileBounds` falls back to the full canvas for a span it cannot
      // bound; turning tight bounds off is the bisection knob for tight-bound regressions.
      SpanTileBounds tileBounds;
      if (config_.tightBoundedSegments) {
        ZoneScopedN("Compositor::segment::computeBounds");
        tileBounds = ComputeSpanTileBounds(renderer(), registry, paintOrder[startIdx],
                                           paintOrder[endIdx], viewport, surfaceFromCanvas);
      }
      const bool visibleInViewport = tileBounds.snapped.has_value();
      const bool offCanvas = tileBounds.offCanvas;
      const bool useTight = tileBounds.useTight;
      const Box2d tightBoundsSnapped = tileBounds.snapped.value_or(Box2d());

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
            config_.immediateStaticSpans && IsStaticSpanImmediateSafe(spanPlan) &&
            presentationCost.redrawCost <= presentationCost.cacheOverheadCost;
      }

      const Vector2i canvasSize = BitmapDimensionsForViewport(viewport);
      if (!offCanvas && adoptBudgetImmediateSpan(i, previousSpanPlan, spanPlan, canvasSize)) {
        continue;
      }
      if (offCanvas) {
        holdEmptySegment(i);
      } else {
        if (surfaceBudgetExhaustedThisFrame_) {
          // The latched budget rejects every further allocation this frame; the remaining dirty
          // slots complete in the next frame.
          return;
        }
        std::unique_ptr<RendererInterface> offscreen;
        {
          ZoneScopedN("Compositor::segment::createOffscreen");
          offscreen = acquireOffscreen();
        }
        UTILS_RELEASE_ASSERT(offscreen != nullptr);
        // A tight slot draws into a crop of the canvas shifted by the crop origin.
        RenderViewport slotViewport = viewport;
        Transform2d slotSurfaceFromCanvas = surfaceFromCanvas;
        Vector2d slotOffset = Vector2d::Zero();
        if (useTight) {
          slotViewport.size = tightBoundsSnapped.size();
          slotSurfaceFromCanvas =
              surfaceFromCanvas * Transform2d::Translate(-tightBoundsSnapped.topLeft);
          slotOffset = tightBoundsSnapped.topLeft;
        }
        {
          ZoneScopedN("Compositor::segment::drawEntityRange");
          if (!rasterizeSegmentSlot(i, std::move(offscreen), paintOrder[startIdx],
                                    paintOrder[endIdx], slotViewport, slotSurfaceFromCanvas,
                                    previousSpanPlan, spanPlan, canvasSize)) {
            return;
          }
        }
        staticSegmentOffsets_[i] = slotOffset;
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
    if (config_.immediateStaticSpans && IsStaticSpanImmediateSafe(spanPlan)) {
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
      if (config_.immediateStaticSpans && !spanPlan.demotedDynamicImmediate &&
          IsBoundedMultiDrawStaticSpan(spanPlan)) {
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
                             !renderer().supportsTextureSnapshotCompositing();
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
    if (!segmentIsEmpty) {
      // After the segment's timing stamp so the yield never inflates the
      // measured rasterize time that dynamic-immediate promotion consumes.
      yieldBetweenTiles();
    }
  }
}

bool CompositorController::staticSegmentStateSized(size_t count) const {
  return staticSegments_.size() == count && staticSegmentTextures_.size() == count &&
         staticSegmentStyleWorkIncomplete_.size() == count && staticSegmentDirty_.size() == count &&
         staticSegmentGeneration_.size() == count && staticSegmentOffsets_.size() == count &&
         staticSegmentLastRasterizeMs_.size() == count && staticSpanPlans_.size() == count;
}

bool CompositorController::resyncSegmentsToLayerSet(const Vector2i& currentCanvasSize,
                                                    const Transform2d& surfaceFromCanvas) {
  const size_t newCount = layers_.size() + 1;

  // Canvas resized - bitmap caches are sized to the old canvas and can't
  // be reused at a different resolution (would be scaled or leave
  // transparent gaps). Full invalidation.
  const bool canvasChanged = staticSegmentsCanvas_ != currentCanvasSize;
  const bool surfaceChanged =
      hasStaticSegmentsSurfaceFromCanvas_ &&
      !SameTransformNear(staticSegmentsSurfaceFromCanvas_, surfaceFromCanvas);

  const bool parallelStateSized = staticSegmentStateSized(newCount);
  bool boundariesUnchanged = staticSegmentBoundaries_.size() == newCount;
  for (size_t i = 0; boundariesUnchanged && i < newCount; ++i) {
    const Entity left = (i == 0) ? entt::null : layers_[i - 1].entity();
    const Entity right = (i == layers_.size()) ? entt::null : layers_[i].entity();
    boundariesUnchanged = staticSegmentBoundaries_[i] == std::pair{left, right};
  }
  if (!canvasChanged && !surfaceChanged && hasStaticSegmentsSurfaceFromCanvas_ &&
      parallelStateSized && boundariesUnchanged) {
    return std::any_of(staticSegmentDirty_.begin(), staticSegmentDirty_.end(),
                       [](bool dirty) { return dirty; });
  }

  // Compute the NEW boundary identity for each slot. Slot i sits between
  // layers_[i-1] and layers_[i]; the edge cases at i==0 and i==N use
  // entt::null as the "beyond the document" sentinel.
  std::vector<std::pair<Entity, Entity>> newBoundaries(newCount);
  for (size_t i = 0; i < newCount; ++i) {
    const Entity left = (i == 0) ? entt::null : layers_[i - 1].entity();
    const Entity right = (i == layers_.size()) ? entt::null : layers_[i].entity();
    newBoundaries[i] = {left, right};
  }

  ++fastPathCounters_.segmentResyncRebuilds;

  std::vector<RendererBitmap> newSegments(newCount);
  std::vector<bool> newStyleWorkIncomplete(newCount, false);
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
          if (j < staticSegmentStyleWorkIncomplete_.size()) {
            newStyleWorkIncomplete[i] = staticSegmentStyleWorkIncomplete_[j];
          }
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
  staticSegmentStyleWorkIncomplete_ = std::move(newStyleWorkIncomplete);
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
    if (!hasPayload) {
      return false;
    }
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
    const RendererBitmap* layerBitmap = PublicLayerBitmap(layer);
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

bool CompositorController::hasCompletePaintOrderTilePayloads() const {
  for (const CompositorLayer& layer : layers_) {
    if (!layer.hasRenderablePayload()) {
      return false;
    }
  }
  for (size_t index = 0; index < staticSpanPlans_.size(); ++index) {
    if (staticSpanPlans_[index].firstEntity == entt::null ||
        staticSpanPlans_[index].lastEntity == entt::null) {
      continue;
    }
    // A placeholder bitmap marks a span with nothing on this raster.
    const bool hasBitmap = index < staticSegments_.size() && !staticSegments_[index].empty();
    const bool hasTexture =
        index < staticSegmentTextures_.size() && staticSegmentTextures_[index] != nullptr;
    if (!hasBitmap && !hasTexture) {
      return false;
    }
  }
  return true;
}

bool CompositorController::hasCompleteTileSetForPresentation() const {
  const size_t segmentCount = layers_.size() + 1u;
  const std::array<size_t, 4> segmentStorageSizes = {
      staticSegments_.size(), staticSegmentTextures_.size(), staticSegmentDirty_.size(),
      staticSpanPlans_.size()};
  if (!std::ranges::all_of(segmentStorageSizes,
                           [segmentCount](size_t size) { return size == segmentCount; })) {
    return false;
  }
  if (lastRenderFrameStats_.textureAllocationFailureCount != 0 ||
      !hasCompletePaintOrderTilePayloads()) {
    return false;
  }
  for (size_t index = 0; index < segmentCount; ++index) {
    const bool hasPayload =
        !staticSegments_[index].empty() || staticSegmentTextures_[index] != nullptr;
    if (!hasPayload &&
        (staticSegmentDirty_[index] || staticSpanPlans_[index].firstEntity != entt::null)) {
      return false;
    }
  }
  return true;
}

bool CompositorController::hasBudgetDirectSlot(const Vector2i& canvasSize) const {
  const bool directLayer = std::ranges::any_of(layers_, [&](const CompositorLayer& layer) {
    return layer.immediatePlan().budgetImmediate &&
           layer.immediatePlan().budgetCanvasSize == canvasSize;
  });
  return directLayer || std::ranges::any_of(staticSpanPlans_, [&](const StaticSpanPlan& plan) {
           return plan.budgetImmediate && plan.budgetCanvasSize == canvasSize &&
                  plan.mode == StaticSpanMode::Immediate;
         });
}

bool CompositorController::layersHavePayloadOrDrawDirectly(const Vector2i& canvasSize) const {
  return std::ranges::all_of(layers_, [&](const CompositorLayer& layer) {
    const ImmediateLayerPlan& plan = layer.immediatePlan();
    return layer.hasRenderablePayload() ||
           (plan.budgetImmediate && plan.budgetCanvasSize == canvasSize);
  });
}

bool CompositorController::spansHavePayloadOrDrawDirectly(const Vector2i& canvasSize) const {
  for (size_t index = 0; index < staticSpanPlans_.size(); ++index) {
    const StaticSpanPlan& plan = staticSpanPlans_[index];
    const bool emptySpan = plan.firstEntity == entt::null || plan.lastEntity == entt::null;
    const bool hasBitmap = index < staticSegments_.size() && !staticSegments_[index].empty();
    const bool hasTexture =
        index < staticSegmentTextures_.size() && staticSegmentTextures_[index] != nullptr;
    const bool drawnDirectly = plan.budgetImmediate && plan.budgetCanvasSize == canvasSize &&
                               plan.mode == StaticSpanMode::Immediate;
    if (!emptySpan && !hasBitmap && !hasTexture && !drawnDirectly) {
      return false;
    }
  }
  return true;
}

void CompositorController::noteFlatFrameDrawn() {
  flatFrameDrawnThisFrame_ = !renderer().resourceStats().surfaceBudgetRejected;
}

bool CompositorController::composedFrameStandsInForTiles() const {
  if (hasCompleteTileSetForPresentation()) {
    return false;
  }
  if (flatFrameDrawnThisFrame_) {
    return true;
  }
  const bool frameComplete = mainFrameComposedThisFrame_ &&
                             lastRenderFrameStats_.textureAllocationFailureCount == 0 &&
                             lastRenderFrameStats_.composePayloadRefusalCount == 0;
  if (!frameComplete || !hasLastViewport_) {
    return false;
  }
  const Vector2i canvasSize = BitmapDimensionsForViewport(lastViewport_);
  return hasBudgetDirectSlot(canvasSize) && layersHavePayloadOrDrawDirectly(canvasSize) &&
         spansHavePayloadOrDrawDirectly(canvasSize);
}

bool CompositorController::beginMainCompose(const RenderViewport& viewport) {
  // Tile-only callers do not consume the main frame. Keep the cold-frame guard and force
  // composition when pixel verification reads the main renderer as its actual image.
  if (canSkipMainCompose(viewport)) {
    return false;
  }
  ++lastRenderFrameStats_.mainComposeCount;
  // The compose replaces any flat frame drawn earlier this frame.
  flatFrameDrawnThisFrame_ = false;
  renderer().beginFrame(viewport);
  if (!frameTargetRefused()) {
    return true;
  }
  // The budget refused the frame target, so there is no frame to draw into: drawing the tiles or
  // the direct slots would write into a missing surface. Nothing is composed this frame.
  renderer().endFrame();
  mainFrameComposedThisFrame_ = false;
  mainRendererHasCachedFrame_ = false;
  return false;
}

bool CompositorController::frameTargetRefused() const {
  const RendererResourceStats stats = renderer().resourceStats();
  return stats.surfaceBudgetSupported && stats.surfaceBudgetRejected;
}

bool CompositorController::canSkipMainCompose(const RenderViewport& viewport) const {
  // A slot presented directly after budget strikes has no tile: only the composed frame shows it.
  return skipMainComposeDuringSplit_ && hasSplitStaticLayers() && mainRendererHasCachedFrame_ &&
         !config_.verifyPixelIdentity &&
         !hasBudgetDirectSlot(BitmapDimensionsForViewport(viewport));
}

void CompositorController::recordComposePayloadRefusal() {
  ++lastRenderFrameStats_.composePayloadRefusalCount;
  ++lastRenderFrameStats_.composePayloadRefusalTotal;
}

bool CompositorController::composePayload(
    const RendererBitmap* bitmap, const std::shared_ptr<const RendererTextureSnapshot>& texture,
    const Transform2d& canvasFromPayload, bool textureMode) {
  const Vector2i payloadDims = ComposePayloadDimensions(bitmap, texture);
  if (payloadDims.x <= 0 || payloadDims.y <= 0) {
    return true;
  }
  ImageParams params;
  params.targetRect = Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(payloadDims.x),
                                                       static_cast<double>(payloadDims.y)));
  // Tile blits are not element draws. Reset per-element paint so they cannot inherit styling
  // from the previous element on a backend that consumes paint state for bitmap payloads.
  renderer().setPaint(PaintParams{});
  renderer().setTransform(canvasFromPayload);
  if (texture != nullptr) {
    if (renderer().drawTextureSnapshot(*texture, params.targetRect)) {
      return true;
    }
    if (textureMode) {
      recordComposePayloadRefusal();
      return false;
    }
  }
  if (bitmap != nullptr && HasPublicTileBitmap(*bitmap)) {
    if (textureMode) {
      // A CPU bitmap in texture mode comes from an offscreen that cannot produce textures, so
      // re-rasterizing would only yield another bitmap; the payload is skipped and counted
      // every frame rather than uploaded every frame, and the tile stays absent.
      recordComposePayloadRefusal();
      return true;
    }
    // drawBitmap consumes the premultiplied raster directly. Routing this
    // through the unpremultiplied ImageResource contract cost two full
    // pixel-buffer conversions plus three allocations per layer/segment per
    // composed frame - the dominant compose cost on drag-heavy replays.
    renderer().drawBitmap(*bitmap, params);
  }
  return true;
}

void CompositorController::releaseLayerPayloadsOfAnotherRaster(
    const Vector2i& canvasSize, const Transform2d& surfaceFromCanvas) {
  for (CompositorLayer& layer : layers_) {
    // A layer with no recorded raster holds no payload: every payload setter records one.
    const std::optional<CompositorLayer::PayloadRaster>& raster = layer.payloadRaster();
    if (!raster.has_value() || SameRasterUpToTranslation(*raster, canvasSize, surfaceFromCanvas)) {
      continue;
    }
    layer.releasePayload();
    layer.markDirty();
  }
}

void CompositorController::markRefusedTilesDirty(const std::vector<Entity>& refusedLayers,
                                                 const std::vector<size_t>& refusedSegments) {
  // A declined texture draw means the tile's backing is no longer presentable; re-rasterize it
  // once rather than falling back to a CPU upload. The strike counts in `composeLayers` bound
  // the retry.
  for (const Entity entity : refusedLayers) {
    if (CompositorLayer* layer = findLayer(entity)) {
      layer->markDirty();
    }
  }
  for (const size_t segmentIndex : refusedSegments) {
    if (segmentIndex < staticSegmentDirty_.size()) {
      staticSegmentDirty_[segmentIndex] = true;
    }
  }
}

bool CompositorController::composePayloadWithProvenance(
    const RendererBitmap* bitmap, const std::shared_ptr<const RendererTextureSnapshot>& texture,
    const Transform2d& canvasFromPayload, bool textureMode, bool styleWorkIncomplete) {
  const bool composed = composePayload(bitmap, texture, canvasFromPayload, textureMode);
  const bool contributes =
      texture != nullptr || (!textureMode && bitmap != nullptr && HasPublicTileBitmap(*bitmap));
  if (composed && contributes && styleWorkIncomplete) {
    composedStyleWorkIncomplete_ = true;
  }
  return composed;
}

void CompositorController::composeLayers(const RenderViewport& viewport,
                                         const Transform2d& surfaceFromCanvas) {
  ZoneScopedN("Compositor::composeLayersImpl");

  const auto composeStart = std::chrono::steady_clock::now();
  if (!beginMainCompose(viewport)) {
    return;
  }

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
    RendererDriver driver(renderer(), false, nullptr, styleWork_);
    driver.drawEntityRangeIntoCurrentFrame(document().registry(), spanPlan.firstEntity,
                                           spanPlan.lastEntity, viewport, surfaceFromCanvas);
    const auto directEnd = std::chrono::steady_clock::now();
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(directEnd - directStart).count();
    lastRenderFrameStats_.immediateRasterizeMs += static_cast<double>(elapsedUs) / 1000.0;
    return true;
  };

  const bool textureMode = renderer().supportsTextureSnapshotCompositing();
  const auto drawPayload = [&](const RendererBitmap* bitmap,
                               const std::shared_ptr<const RendererTextureSnapshot>& texture,
                               const Transform2d& canvasFromPayload, bool styleWorkIncomplete) {
    return composePayloadWithProvenance(bitmap, texture, canvasFromPayload, textureMode,
                                        styleWorkIncomplete);
  };

  if (staticSegmentComposeDeclines_.size() != staticSegments_.size()) {
    staticSegmentComposeDeclines_.assign(staticSegments_.size(), 0);
  }
  std::vector<Entity> refusedLayers;
  std::vector<size_t> refusedSegments;
  const auto drawLayer = [&](CompositorLayer& layer) {
    // A clean immediate drag layer may deliberately retain its raster while a pure translation
    // moves `canvasFromBitmap`. Direct-drawing that layer can reproduce its old pixels in the
    // flattened frame, so translate the retained payload below. Scale and rotation keep the
    // established direct-render path, which avoids resampling differences from transforming an
    // already-rasterized payload. That resampling guarantee is preserved for isolated-effect
    // layers too: they compose from their cached bitmap when the bitmap sits at its rendered
    // transform, but a rotate or scale drag re-renders them rather than resampling the payload
    // through a non-translation affine.
    // isTranslation() is true for identity, so one predicate covers both
    // at-rest cases.
    const bool nonTranslationTransform = !layer.canvasFromBitmap().isTranslation();
    const bool resampleSensitiveEffect =
        (layer.fallbackReasons() & (FallbackReason::Filter | FallbackReason::ClipPath |
                                    FallbackReason::Mask | FallbackReason::IsolatedLayer)) !=
        FallbackReason::None;
    const bool shouldDirectComposeLayer =
        ShouldDirectComposeLayer(layer) || (nonTranslationTransform && resampleSensitiveEffect);
    const bool blitRetainedTranslation = shouldDirectComposeLayer && layer.hasRenderablePayload() &&
                                         !layer.canvasFromBitmap().isIdentity() &&
                                         layer.canvasFromBitmap().isTranslation();
    if (shouldDirectComposeLayer && !blitRetainedTranslation) {
      const auto directStart = std::chrono::steady_clock::now();
      RendererDriver driver(renderer(), false, nullptr, styleWork_);
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
    if (drawPayload(bitmap, layer.textureSnapshot(), canvasFromBitmap,
                    layer.styleWorkIncomplete())) {
      layer.clearComposeDeclines();
    } else if (layer.recordComposeDecline() < 2) {
      // One re-rasterize recovers a texture the renderer can draw after a device change; a
      // second decline of the fresh payload will not converge, so stop paying for it.
      refusedLayers.push_back(layer.entity());
    }
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
    if (drawPayload(&staticSegments_[segmentIndex], segmentTexture,
                    Transform2d::Translate(segmentOffset),
                    staticSegmentStyleWorkIncomplete_[segmentIndex])) {
      staticSegmentComposeDeclines_[segmentIndex] = 0;
    } else if (staticSegmentComposeDeclines_[segmentIndex] < 2 &&
               ++staticSegmentComposeDeclines_[segmentIndex] < 2) {
      refusedSegments.push_back(segmentIndex);
    }
  };

  // Compose static segments and promoted layers in
  // interleaved paint order. Active drag frames may skip this main-renderer
  // compose via the `skipMainCompose` gate above.
  if (!staticSegments_.empty()) {
    for (size_t i = 0; i < layers_.size(); ++i) {
      drawSegment(i);
      drawLayer(layers_[i]);
    }
    drawSegment(staticSegments_.size() - 1u);
  }
  markRefusedTilesDirty(refusedLayers, refusedSegments);

  renderer().endFrame();
  // A refused frame target latches the shared budget, so a frame composed without one reads as
  // rejected here.
  mainFrameComposedThisFrame_ = !renderer().resourceStats().surfaceBudgetRejected;
  lastRenderFrameStats_.mainComposeMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - composeStart)
          .count();
  mainRendererHasCachedFrame_ = true;
}

}  // namespace donner::svg::compositor
