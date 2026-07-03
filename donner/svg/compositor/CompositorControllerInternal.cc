/// @file
/// Implementation of the CompositorController helper seam: layer raster
/// geometry, DOM-tree ancestry checks, and the static-span cost /
/// immediate-promotion heuristics.

#include "donner/svg/compositor/CompositorControllerInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <variant>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg::compositor {

namespace {

bool IsTransparentPlaceholderBitmap(const RendererBitmap& bitmap) {
  return bitmap.dimensions == Vector2i(1, 1) && bitmap.pixels.size() == 4u &&
         std::all_of(bitmap.pixels.begin(), bitmap.pixels.end(),
                     [](uint8_t channel) { return channel == 0u; });
}

Entity DataEntityForPaintEntity(const Registry& registry, Entity entity) {
  if (const auto* instance = registry.try_get<components::RenderingInstanceComponent>(entity)) {
    return instance->dataEntity;
  }

  return entity;
}

std::string EntityDebugLabel(const Registry& registry, Entity entity) {
  if (entity == entt::null || !registry.valid(entity)) {
    return "none";
  }

  const Entity dataEntity = DataEntityForPaintEntity(registry, entity);
  std::string label;
  if (const auto* tree = registry.try_get<donner::components::TreeComponent>(dataEntity)) {
    label = std::string(tree->tagName().name);
  } else {
    label = "entity";
  }

  if (const auto* id = registry.try_get<components::IdComponent>(dataEntity); id != nullptr) {
    const RcString value = id->id();
    if (!value.empty()) {
      label += "#";
      label += std::string(value);
    }
  }

  label += " #";
  label += std::to_string(static_cast<unsigned>(dataEntity));
  return label;
}

}  // namespace

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

Vector2i LayerPayloadDimensions(const CompositorLayer& layer) {
  if (layer.hasValidBitmap()) {
    return layer.bitmap().dimensions;
  }
  if (layer.textureSnapshot() != nullptr) {
    return layer.textureSnapshot()->dimensions();
  }
  return Vector2i::Zero();
}

Vector2i BitmapDimensionsForViewport(const RenderViewport& viewport) {
  return Vector2i(static_cast<int>(viewport.size.x * viewport.devicePixelRatio),
                  static_cast<int>(viewport.size.y * viewport.devicePixelRatio));
}

LayerRasterGeometry ComputeLayerRasterGeometry(RendererInterface& renderer, Registry& registry,
                                               Entity firstEntity, Entity lastEntity,
                                               const RenderViewport& viewport,
                                               const Transform2d& surfaceFromCanvas) {
  LayerRasterGeometry result;
  result.viewport = viewport;
  result.surfaceFromCanvas = surfaceFromCanvas;

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
                                                              viewport, surfaceFromCanvas);
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
      result.boundsCanvas = tightBoundsSnapped;
      result.tight = tightBoundsSnapped.width() < viewport.size.x ||
                     tightBoundsSnapped.height() < viewport.size.y;
    }
  }

  if (result.tight) {
    result.viewport.size = tightBoundsSnapped.size();
    result.surfaceFromCanvas =
        surfaceFromCanvas * Transform2d::Translate(-tightBoundsSnapped.topLeft);
    result.canvasOffset = tightBoundsSnapped.topLeft;
  }

  return result;
}

bool SameTransformNear(const Transform2d& lhs, const Transform2d& rhs) {
  for (size_t i = 0; i < 6; ++i) {
    if (!NearEquals(lhs.data[i], rhs.data[i])) {
      return false;
    }
  }
  return true;
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

bool PaintUsesExpensiveResource(const components::ResolvedPaintServer& paint) {
  const auto* ref = std::get_if<components::PaintResolvedReference>(&paint);
  if (ref == nullptr) {
    return false;
  }
  if (!ref->reference.valid()) {
    return true;
  }

  // Gradients are still a direct path paint in both renderer backends, so they
  // are not hard-blocked from immediate presentation. Their per-pixel cost is
  // instead folded into the immediate rasterize-cost estimate as an area term
  // (`PaintIsAreaCostly` + `EstimateStaticSpanRasterizeMs`), so a small gradient
  // span can go immediate while a large one stays cached. Pattern paints and
  // unresolved/unknown references may instantiate subtrees or need broader
  // context, so keep those fully cached.
  return ref->subtreeInfo.has_value() ||
         ref->reference.handle.try_get<components::ComputedGradientComponent>() == nullptr;
}

// True when @p paint is a resolved gradient — a fill whose rasterize cost scales
// with covered pixel area (the shader evaluates per pixel) rather than with the
// span's draw-op / path-verb counts. Used to add an area term to the immediate
// rasterize estimate so a large gradient stays cached while a small one does not.
// Patterns / unresolved references are already handled by
// `PaintUsesExpensiveResource` (they force a cached tile outright).
bool PaintIsAreaCostly(const components::ResolvedPaintServer& paint) {
  const auto* ref = std::get_if<components::PaintResolvedReference>(&paint);
  if (ref == nullptr || !ref->reference.valid() || ref->subtreeInfo.has_value()) {
    return false;
  }
  return ref->reference.handle.try_get<components::ComputedGradientComponent>() != nullptr;
}

bool IsNonDrawingContainer(const EntityHandle& dataHandle) {
  const auto* type = dataHandle.try_get<components::ElementTypeComponent>();
  if (type == nullptr) {
    return false;
  }

  switch (type->type()) {
    case ElementType::A:
    case ElementType::Defs:
    case ElementType::G:
    case ElementType::SVG:
    case ElementType::Switch:
    case ElementType::Symbol: return true;
    default: return false;
  }
}

void AccumulateStaticSpanCost(Registry& registry, Entity entity, StaticSpanCostEstimate* estimate,
                              bool ignoreIsolatedLayer) {
  const auto* instance = registry.try_get<components::RenderingInstanceComponent>(entity);
  if (instance == nullptr) {
    estimate->hasExpensiveEffect = true;
    return;
  }

  const auto* style = instance->styleHandle(registry).try_get<components::ComputedStyleComponent>();
  if (style == nullptr || !style->properties.has_value()) {
    return;
  }
  if (!instance->visible || style->properties->display.get().value() == Display::None) {
    return;
  }

  if ((!ignoreIsolatedLayer && instance->isolatedLayer) || instance->resolvedFilter.has_value() ||
      instance->clipPath.has_value() || (instance->mask.has_value() && instance->mask->valid()) ||
      instance->markerStart.has_value() || instance->markerMid.has_value() ||
      instance->markerEnd.has_value() || PaintUsesExpensiveResource(instance->resolvedFill) ||
      PaintUsesExpensiveResource(instance->resolvedStroke)) {
    estimate->hasExpensiveEffect = true;
  }
  if (PaintIsAreaCostly(instance->resolvedFill) || PaintIsAreaCostly(instance->resolvedStroke)) {
    estimate->usesAreaCostlyPaint = true;
  }

  EntityHandle dataHandle = instance->dataHandle(registry);
  if (const auto* path = dataHandle.try_get<components::ComputedPathComponent>()) {
    ++estimate->drawOps;
    estimate->pathVerbs += static_cast<int>(path->spline.verbCount());
    return;
  }

  if (dataHandle.try_get<components::ComputedTextComponent>() ||
      dataHandle.try_get<components::LoadedImageComponent>() ||
      dataHandle.try_get<components::LoadedSVGImageComponent>() ||
      dataHandle.try_get<components::ExternalUseComponent>()) {
    estimate->hasExpensiveEffect = true;
    return;
  }

  if (IsNonDrawingContainer(dataHandle)) {
    return;
  }

  if (!instance->subtreeInfo.has_value()) {
    estimate->hasExpensiveEffect = true;
  }
}

StaticSpanCostEstimate EstimateStaticSpanCost(Registry& registry,
                                              const std::vector<Entity>& paintOrder,
                                              size_t startIdx, size_t endIdx) {
  StaticSpanCostEstimate estimate;
  for (size_t i = startIdx; i <= endIdx && i < paintOrder.size(); ++i) {
    AccumulateStaticSpanCost(registry, paintOrder[i], &estimate, /*ignoreIsolatedLayer=*/false);
  }
  return estimate;
}

StaticSpanCostEstimate EstimateEntityRangeCost(Registry& registry, Entity firstEntity,
                                               Entity lastEntity) {
  StaticSpanCostEstimate estimate;
  RenderingInstanceView view(registry);
  while (!view.done() && view.currentEntity() != firstEntity) {
    view.advance();
  }
  while (!view.done()) {
    const Entity currentEntity = view.currentEntity();
    AccumulateStaticSpanCost(registry, currentEntity, &estimate,
                             /*ignoreIsolatedLayer=*/currentEntity == firstEntity);
    if (currentEntity == lastEntity) {
      break;
    }
    view.advance();
  }
  return estimate;
}

uint64_t EstimateStaticSpanRetainedBytes(const Box2d& boundsCanvas) {
  const double width = std::ceil(std::max(0.0, boundsCanvas.width()));
  const double height = std::ceil(std::max(0.0, boundsCanvas.height()));
  return static_cast<uint64_t>(width * height) * 4u;
}

StaticSpanPresentationCost EstimateStaticSpanPresentationCost(
    const StaticSpanCostEstimate& spanCost, const Box2d& boundsCanvas) {
  const uint64_t retainedBytes = EstimateStaticSpanRetainedBytes(boundsCanvas);
  const double retainedPixels = static_cast<double>(retainedBytes) / 4.0;

  // Relative cost model: compare rerendering a simple span against the fixed
  // bookkeeping and retained-memory cost of keeping a presentation texture.
  constexpr double kRasterPixelCost = 1.0;
  constexpr double kRasterDrawOpCost = 16.0;
  constexpr double kRasterPathVerbCost = 4.0;
  constexpr double kCachedTextureFixedCost = 512.0;
  constexpr double kRetainedByteCost = 1.0 / 16.0;

  return StaticSpanPresentationCost{
      .retainedBytes = retainedBytes,
      .redrawCost = retainedPixels * kRasterPixelCost +
                    static_cast<double>(spanCost.drawOps) * kRasterDrawOpCost +
                    static_cast<double>(spanCost.pathVerbs) * kRasterPathVerbCost,
      .cacheOverheadCost =
          kCachedTextureFixedCost + static_cast<double>(retainedBytes) * kRetainedByteCost,
  };
}

bool IsImmediateSafe(bool visible, bool hasExpensiveEffect, int estimatedDrawOps) {
  return visible && !hasExpensiveEffect && estimatedDrawOps > 0;
}

bool IsCheapDirectGeometry(const StaticSpanCostEstimate& cost) {
  constexpr int kCheapDirectDrawOps = 2;
  constexpr int kCheapDirectPathVerbs = 96;
  return cost.drawOps > 0 && cost.drawOps <= kCheapDirectDrawOps &&
         cost.pathVerbs <= kCheapDirectPathVerbs;
}

// Deterministic, forward-looking estimate (milliseconds) of how long it takes to
// re-rasterize a non-promoted span (or cheap promoted layer) from its geometry.
//
// This replaces the previous *measured* per-render elapsed sample in the
// immediate-vs-cached promotion decision. Measuring the wall clock and then
// promoting if it happened to be fast made the decision flap with machine load:
// the same span rendered immediate on an idle machine and cached under a busy
// one, so editor presentation — and any test that asserted on it — was
// runner-sensitive. Predicting the cost from the span's geometry instead makes
// the choice identical on every machine.
//
// The model is dispatch-overhead-dominated for small spans (offscreen creation +
// driver setup) plus a per-draw-op and per-path-verb term. Pixel-fill *area* is
// intentionally excluded: fill is cheap on both backends and its cost belongs to
// the cache/retained-memory side of the tradeoff (`cacheOverheadCost`), not the
// redraw side. The constants are a first-order fit; the planner still records the
// real `measuredRasterizeMs` for every span so the model can be recalibrated
// against observed timings without reintroducing it into the decision.
double EstimateStaticSpanRasterizeMs(int estimatedDrawOps, int estimatedPathVerbs,
                                     bool usesAreaCostlyPaint, double coveredAreaPixels) {
  constexpr double kBaseDispatchMs = 0.25;
  constexpr double kMsPerDrawOp = 0.05;
  constexpr double kMsPerPathVerb = 0.004;
  double ms = kBaseDispatchMs + static_cast<double>(std::max(0, estimatedDrawOps)) * kMsPerDrawOp +
              static_cast<double>(std::max(0, estimatedPathVerbs)) * kMsPerPathVerb;
  if (usesAreaCostlyPaint) {
    // A gradient fill evaluates its shader at every covered pixel, so unlike a
    // solid fill its cost scales with area. Charge per covered kilopixel so a
    // small gradient span (a letter accent) can still go immediate while a large
    // one (a full-width cloud band) estimates over budget and stays cached —
    // which is also what keeps a dragged gradient layer on the cached
    // texture-transform path instead of re-rasterizing every frame.
    // Calibrated against real splash gradient renders: the DONNER logo span
    // (~98 kpx) rasterizes in ~1.9 ms and must stay under the ~2.08 ms budget
    // (immediate), while #Blue_center_burst (~457 kpx) takes ~8 ms and must
    // exceed it (cached). That brackets the per-kilopixel rate to ~0.0025-0.0083;
    // 0.005 sits in the middle. Gradient cost isn't perfectly area-linear, but the
    // immediate-vs-cached decision only needs the right side of the budget, and
    // since the #633 paint-leak fix a misclassification costs at most a little
    // perf, never pixels.
    constexpr double kMsPerAreaCostlyKilopixel = 0.005;
    ms += std::max(0.0, coveredAreaPixels) / 1000.0 * kMsPerAreaCostlyKilopixel;
  }
  return ms;
}

bool ShouldDirectComposeLayer(const CompositorLayer& layer) {
  return layer.isImmediate() ||
         (layer.fallbackReasons() &
          (FallbackReason::ExternalPaint | FallbackReason::IsolatedLayer)) != FallbackReason::None;
}

double ImmediateStaticSpanBudgetMs() {
  // 120 Hz leaves 8.33 ms total. Immediate spans rerender on the UI cadence,
  // so dynamic expansion gets one quarter of that frame to leave room for
  // input, presentation, and editor chrome.
  return (1000.0 / 120.0) * 0.25;
}

double ImmediateStaticSpanBudgetChargeMs(double measuredRasterizeMs) {
  constexpr double kMinimumChargeMs = 0.05;
  if (!(measuredRasterizeMs >= 0.0) || !std::isfinite(measuredRasterizeMs)) {
    return ImmediateStaticSpanBudgetMs();
  }
  return std::max(measuredRasterizeMs, kMinimumChargeMs);
}

bool IsStaticSpanImmediateSafe(const CompositorController::StaticSpanPlan& spanPlan) {
  return IsImmediateSafe(spanPlan.visible, spanPlan.hasExpensiveEffect, spanPlan.estimatedDrawOps);
}

bool IsBoundedMultiDrawStaticSpan(const CompositorController::StaticSpanPlan& spanPlan) {
  constexpr uint64_t kDirectStaticSpanMinBytes = 512u * 1024u;
  constexpr uint64_t kDirectStaticSpanMaxBytes = 720u * 1024u;
  return IsStaticSpanImmediateSafe(spanPlan) && spanPlan.estimatedDrawOps > 1 &&
         spanPlan.estimatedRetainedBytes >= kDirectStaticSpanMinBytes &&
         spanPlan.estimatedRetainedBytes <= kDirectStaticSpanMaxBytes;
}

// Empty paint-order gaps are cached internally as transparent 1x1 bitmaps so
// `RendererBitmap::empty()` can keep meaning "not rasterized yet". Public tile
// snapshots and composition should ignore those bookkeeping placeholders.
bool HasPublicTileBitmap(const RendererBitmap& bitmap) {
  return !bitmap.empty() && !IsTransparentPlaceholderBitmap(bitmap);
}

// Returns true when two bitmaps are byte-for-byte identical (same format,
// dimensions, stride, and pixel bytes). Used to detect a no-op re-rasterize
// of a static segment so its tile generation — the editor's GL-texture-cache
// invalidation key — isn't advanced when nothing actually changed.
bool BitmapPixelsEqual(const RendererBitmap& a, const RendererBitmap& b) {
  return a.dimensions == b.dimensions && a.rowBytes == b.rowBytes && a.alphaType == b.alphaType &&
         a.pixels == b.pixels;
}

std::string SpanRangeLabel(const Registry& registry, Entity firstEntity, Entity lastEntity) {
  if (firstEntity == entt::null) {
    return "";
  }

  const std::string firstLabel = EntityDebugLabel(registry, firstEntity);
  if (lastEntity == entt::null || lastEntity == firstEntity) {
    return firstLabel;
  }

  return firstLabel + " → " + EntityDebugLabel(registry, lastEntity);
}

}  // namespace donner::svg::compositor
