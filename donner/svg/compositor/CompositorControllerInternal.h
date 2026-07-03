#pragma once
/// @file
/// Shared helpers behind `CompositorController`: layer raster geometry,
/// DOM-tree ancestry checks, and the static-span cost/immediate-promotion
/// heuristics. Split out of CompositorController.cc so the controller file
/// stays focused on layer lifecycle, rasterization, and composition.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor {

/// @cond INTERNAL

/// Raster geometry for one layer/segment: the viewport to rasterize with and
/// where the resulting bitmap lands on the canvas.
struct LayerRasterGeometry {
  RenderViewport viewport;
  Transform2d surfaceFromCanvas;
  Vector2d canvasOffset = Vector2d::Zero();
  std::optional<Box2d> boundsCanvas;
  bool tight = false;
};

/// Deterministic cost estimate for a static paint-order span.
struct StaticSpanCostEstimate {
  int drawOps = 0;
  int pathVerbs = 0;
  bool hasExpensiveEffect = false;
  /// True when any draw in the span fills with a gradient, whose rasterize cost
  /// scales with covered area. Feeds the area term of the immediate estimate.
  bool usesAreaCostlyPaint = false;
};

/// Cached-tile vs redraw cost trade-off for a static span.
struct StaticSpanPresentationCost {
  uint64_t retainedBytes = 0;
  double redrawCost = 0.0;
  double cacheOverheadCost = 0.0;
};

bool HasCompositingBreakingAncestor(Registry& registry, Entity entity);
bool IsDomDescendantOf(Registry& registry, Entity maybeDescendant, Entity root);
bool IsEntityInLiveTree(Registry& registry, Entity entity, Entity rootEntity);

Vector2i LayerPayloadDimensions(const CompositorLayer& layer);
Vector2i BitmapDimensionsForViewport(const RenderViewport& viewport);
LayerRasterGeometry ComputeLayerRasterGeometry(RendererInterface& renderer, Registry& registry,
                                               Entity firstEntity, Entity lastEntity,
                                               const RenderViewport& viewport,
                                               const Transform2d& surfaceFromCanvas);
bool SameTransformNear(const Transform2d& lhs, const Transform2d& rhs);
bool IsIntegerTranslation(const Transform2d& transform, Vector2d* roundedTranslation);

StaticSpanCostEstimate EstimateStaticSpanCost(Registry& registry,
                                              const std::vector<Entity>& paintOrder,
                                              size_t startIdx, size_t endIdx);
StaticSpanCostEstimate EstimateEntityRangeCost(Registry& registry, Entity firstEntity,
                                               Entity lastEntity);
StaticSpanPresentationCost EstimateStaticSpanPresentationCost(
    const StaticSpanCostEstimate& spanCost, const Box2d& boundsCanvas);
bool IsImmediateSafe(bool visible, bool hasExpensiveEffect, int estimatedDrawOps);
bool IsCheapDirectGeometry(const StaticSpanCostEstimate& cost);
double EstimateStaticSpanRasterizeMs(int estimatedDrawOps, int estimatedPathVerbs,
                                     bool usesAreaCostlyPaint, double coveredAreaPixels);
bool ShouldDirectComposeLayer(const CompositorLayer& layer);
double ImmediateStaticSpanBudgetMs();
double ImmediateStaticSpanBudgetChargeMs(double measuredRasterizeMs);

bool IsStaticSpanImmediateSafe(const CompositorController::StaticSpanPlan& spanPlan);
bool IsBoundedMultiDrawStaticSpan(const CompositorController::StaticSpanPlan& spanPlan);
bool HasPublicTileBitmap(const RendererBitmap& bitmap);
bool BitmapPixelsEqual(const RendererBitmap& a, const RendererBitmap& b);
std::string SpanRangeLabel(const Registry& registry, Entity firstEntity, Entity lastEntity);

/// Tile-id bit distinguishing promoted-layer tiles from static-segment tiles
/// in `snapshotTilesForUpload`.
inline constexpr uint64_t kLayerTileBit = 1ull << 63;

/// @endcond

}  // namespace donner::svg::compositor
