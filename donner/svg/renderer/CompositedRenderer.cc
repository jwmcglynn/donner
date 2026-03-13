#include "donner/svg/renderer/CompositedRenderer.h"

#include <cassert>
#include <iostream>

#include <cmath>

#include "donner/svg/components/LayerMembershipComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/renderer/LayerDecomposition.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg {

namespace {

/// Compute the visual bounding box for a layer's entity range in viewport coordinates.
/// Falls back to the full canvas bounds if no geometry is found.
Boxd computeLayerBounds(Registry& registry, Entity firstEntity, Entity lastEntity,
                        const Vector2i& canvasSize) {
  const Boxd canvasBounds = Boxd::WithSize(Vector2d(canvasSize.x, canvasSize.y));
  std::optional<Boxd> bounds;

  RenderingInstanceView view(registry);

  // Advance to first entity.
  while (!view.done() && view.currentEntity() != firstEntity) {
    view.advance();
  }

  // Accumulate bounds for all entities in the range.
  bool reachedLast = false;
  while (!view.done() && !reachedLast) {
    reachedLast = (view.currentEntity() == lastEntity);
    const auto& instance = view.get();
    view.advance();

    // Get the entity's computed path bounds in local space.
    const auto* path =
        instance.dataHandle(registry).try_get<components::ComputedPathComponent>();
    if (path != nullptr && !path->spline.empty()) {
      // Transform local bounds to viewport coordinates via entityFromWorldTransform.
      const Boxd localBounds = path->spline.bounds();
      const Boxd viewportBounds = instance.entityFromWorldTransform.transformBox(localBounds);

      // Inflate by stroke width (transformed to viewport space as well).
      const auto& style =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();
      double inflation = 0.0;
      if (style.properties.has_value()) {
        inflation = style.properties->strokeWidth.getRequired().value * 0.5;
      }

      const Boxd inflated = inflation > 0.0 ? viewportBounds.inflatedBy(inflation) : viewportBounds;

      if (bounds.has_value()) {
        bounds->addPoint(inflated.topLeft);
        bounds->addPoint(inflated.bottomRight);
      } else {
        bounds = inflated;
      }
    }
  }

  if (!bounds.has_value()) {
    return canvasBounds;
  }

  // Clamp to canvas and round to integer pixels (expand outward).
  Boxd result = *bounds;
  result.topLeft.x = std::max(std::floor(result.topLeft.x), 0.0);
  result.topLeft.y = std::max(std::floor(result.topLeft.y), 0.0);
  result.bottomRight.x =
      std::min(std::ceil(result.bottomRight.x), static_cast<double>(canvasSize.x));
  result.bottomRight.y =
      std::min(std::ceil(result.bottomRight.y), static_cast<double>(canvasSize.y));

  // Ensure minimum 1x1 size.
  if (result.width() < 1.0 || result.height() < 1.0) {
    return canvasBounds;
  }

  return result;
}

}  // namespace

CompositedRenderer::CompositedRenderer(RendererInterface& renderer, bool verbose)
    : renderer_(renderer), verbose_(verbose) {}

CompositedRenderer::~CompositedRenderer() = default;

void CompositedRenderer::prepare(SVGDocument& document,
                                 const std::vector<Entity>& promotedEntities,
                                 CompositingLayer::Reason reason) {
  document_ = &document;

  // Prepare the render tree (styles, layout, resources).
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i newCanvasSize = document.canvasSize();

  // Check if we can do an incremental update (same promotion set, same canvas size).
  const bool samePromotionSet = (promotedEntities == lastPromotedEntities_ &&
                                 reason == lastReason_ && newCanvasSize == canvasSize_ &&
                                 !layers_.layers.empty());

  canvasSize_ = newCanvasSize;

  if (samePromotionSet) {
    // Incremental path: recompute bounds, mark layers dirty only if bounds changed.
    for (auto& layer : layers_.layers) {
      const Boxd oldBounds = layer.bounds;
      layer.bounds = computeLayerBounds(document.registry(), layer.firstEntity, layer.lastEntity,
                                        canvasSize_);

      // If bounds changed, the pixmap dimensions may differ — mark dirty.
      if (layer.bounds != oldBounds) {
        layerCaches_[layer.id].dirty = true;
        layerCaches_[layer.id].imageDirty = true;
      }
    }
  } else {
    // Full rebuild: decompose the render tree into compositing layers.
    layers_ = decomposeIntoLayers(document.registry(), promotedEntities, reason);

    // Compute content-sized bounds for each layer.
    for (auto& layer : layers_.layers) {
      layer.bounds = computeLayerBounds(document.registry(), layer.firstEntity, layer.lastEntity,
                                        canvasSize_);
    }

    // Initialize layer caches — all dirty on first prepare.
    layerCaches_.clear();
    layerCaches_.resize(layers_.layers.size());

    lastPromotedEntities_ = promotedEntities;
    lastReason_ = reason;
    compositionDirty_ = true;
  }

  lastFrameStats_ = {};
}

void CompositedRenderer::renderFrame() {
  assert(document_ != nullptr && "Must call prepare() before renderFrame()");

  FrameStats stats;

  // Rasterize any dirty layers.
  for (size_t i = 0; i < layers_.layers.size(); ++i) {
    if (layerCaches_[i].dirty) {
      // Track offscreen pool hits.
      if (layerCaches_[i].offscreen) {
        ++stats.offscreenPoolHits;
      }
      rasterizeLayer(layers_.layers[i]);
      layerCaches_[i].dirty = false;
      ++stats.layersRasterized;
      compositionDirty_ = true;
    } else {
      ++stats.layersReused;
      // Clean layers reuse their cached ImageResource during composition.
      if (!layerCaches_[i].imageDirty) {
        ++stats.imagePoolHits;
      }
    }
  }

  lastFrameStats_ = stats;

  // Compose layer pixmaps into the final output (skipped if nothing changed).
  if (compositionDirty_) {
    composeLayers();
    compositionDirty_ = false;
  }
}

void CompositedRenderer::composeOnly() {
  assert(document_ != nullptr && "Must call prepare() before composeOnly()");
  composeLayers();
}

void CompositedRenderer::markLayerDirty(uint32_t layerId) {
  if (layerId < layerCaches_.size()) {
    layerCaches_[layerId].dirty = true;
  }
}

void CompositedRenderer::markAllDirty() {
  for (auto& cache : layerCaches_) {
    cache.dirty = true;
  }
}

bool CompositedRenderer::markEntityDirty(Entity entity) {
  auto layerId = findLayerForEntity(entity);
  if (layerId.has_value()) {
    markLayerDirty(*layerId);
    return true;
  }
  return false;
}

void CompositedRenderer::setLayerTransform(uint32_t layerId, const Transformd& transform) {
  if (layerId < layers_.layers.size()) {
    layers_.layers[layerId].compositionTransform = transform;
    compositionDirty_ = true;
  }
}

bool CompositedRenderer::setEntityLayerTransform(Entity entity, const Transformd& transform) {
  auto layerId = findLayerForEntity(entity);
  if (layerId.has_value()) {
    setLayerTransform(*layerId, transform);
    return true;
  }
  return false;
}

void CompositedRenderer::setLayerOpacity(uint32_t layerId, double opacity) {
  if (layerId < layers_.layers.size()) {
    layers_.layers[layerId].compositionOpacity = opacity;
    compositionDirty_ = true;
  }
}

bool CompositedRenderer::setEntityLayerOpacity(Entity entity, double opacity) {
  auto layerId = findLayerForEntity(entity);
  if (layerId.has_value()) {
    setLayerOpacity(*layerId, opacity);
    return true;
  }
  return false;
}

std::optional<uint32_t> CompositedRenderer::findLayerForEntity(Entity entity) const {
  assert(document_ != nullptr && "Must call prepare() before findLayerForEntity()");

  auto& registry = document_->registry();

  // Check if the entity directly has a LayerMembershipComponent.
  const auto* membership = registry.try_get<components::LayerMembershipComponent>(entity);
  if (membership != nullptr) {
    return membership->layerId;
  }

  // Search render entities for a match on dataEntity.
  auto view = registry.view<components::RenderingInstanceComponent,
                            components::LayerMembershipComponent>();
  for (auto renderEntity : view) {
    const auto& instance = view.get<components::RenderingInstanceComponent>(renderEntity);
    if (instance.dataEntity == entity) {
      return view.get<components::LayerMembershipComponent>(renderEntity).layerId;
    }
  }

  return std::nullopt;
}

uint32_t CompositedRenderer::invalidateAnimatedLayers() {
  assert(document_ != nullptr && "Must call prepare() before invalidateAnimatedLayers()");

  auto& registry = document_->registry();
  uint32_t dirtyCount = 0;

  // Find all entities with active animated values and mark their layers dirty.
  for (auto [entity, animValues] :
       registry.view<components::AnimatedValuesComponent>().each()) {
    if (!animValues.overrides.empty()) {
      if (markEntityDirty(entity)) {
        ++dirtyCount;
      }
    }
  }

  return dirtyCount;
}

uint32_t CompositedRenderer::renderPredicted() {
  assert(document_ != nullptr && "Must call prepare() before renderPredicted()");

  const size_t numLayers = layers_.layers.size();

  // Initialize predicted buffers if needed.
  predicted_.bitmaps.resize(numLayers);
  predicted_.valid.assign(numLayers, false);

  uint32_t rendered = 0;
  for (size_t i = 0; i < numLayers; ++i) {
    if (layerCaches_[i].dirty) {
      // Save the current bitmap so rasterizeLayer doesn't clobber it.
      RendererBitmap savedBitmap = std::move(layerCaches_[i].bitmap);

      rasterizeLayer(layers_.layers[i]);

      // Move the newly rasterized bitmap to the predicted buffer.
      predicted_.bitmaps[i] = std::move(layerCaches_[i].bitmap);
      predicted_.valid[i] = true;

      // Restore the current bitmap and keep the layer dirty.
      layerCaches_[i].bitmap = std::move(savedBitmap);
      layerCaches_[i].dirty = true;
      ++rendered;
    }
  }

  return rendered;
}

void CompositedRenderer::swapPredicted() {
  for (size_t i = 0; i < predicted_.valid.size(); ++i) {
    if (predicted_.valid[i]) {
      layerCaches_[i].bitmap = std::move(predicted_.bitmaps[i]);
      layerCaches_[i].dirty = false;
      predicted_.valid[i] = false;
    }
  }
}

RendererBitmap CompositedRenderer::takeSnapshot() const {
  return renderer_.takeSnapshot();
}

void CompositedRenderer::rasterizeLayer(CompositingLayer& layer) {
  assert(document_ != nullptr);

  auto& cache = layerCaches_[layer.id];

  // Reuse cached offscreen renderer if available, otherwise create one.
  if (!cache.offscreen) {
    cache.offscreen = renderer_.createOffscreenInstance();
    if (!cache.offscreen) {
      if (verbose_) {
        std::cerr << "[CompositedRenderer] Backend does not support offscreen rendering\n";
      }
      return;
    }
  }

  // Set up viewport sized to the layer's content bounds.
  const Boxd& bounds = layer.bounds;
  RenderViewport viewport;
  viewport.size = Vector2d(bounds.width(), bounds.height());
  viewport.devicePixelRatio = 1.0;

  // Translate so layer content starts at (0,0) in the pixmap.
  const Transformd layerBaseTransform = Transformd::Translate(-bounds.topLeft.x, -bounds.topLeft.y);

  // Render the layer's entity range into the offscreen renderer.
  RendererDriver layerDriver(*cache.offscreen, verbose_);
  layerDriver.drawEntityRange(document_->registry(), layer.firstEntity, layer.lastEntity, viewport,
                              layerBaseTransform);

  // Capture the rasterized layer content.
  cache.bitmap = cache.offscreen->takeSnapshot();
  cache.imageDirty = true;

  if (verbose_) {
    std::cerr << "[CompositedRenderer] Rasterized layer " << layer.id << " ("
              << cache.bitmap.dimensions.x << "x" << cache.bitmap.dimensions.y << ")\n";
  }
}

void CompositedRenderer::rebuildCachedImage(LayerCache& cache) {
  const auto& bitmap = cache.bitmap;

  cache.cachedImage.width = bitmap.dimensions.x;
  cache.cachedImage.height = bitmap.dimensions.y;

  // Copy pixel data, handling potential row padding.
  const size_t expectedRowBytes = static_cast<size_t>(bitmap.dimensions.x) * 4;
  if (bitmap.rowBytes == expectedRowBytes || bitmap.rowBytes == 0) {
    // No padding — direct copy.
    cache.cachedImage.data = bitmap.pixels;
  } else {
    // Row padding — copy row by row.
    cache.cachedImage.data.resize(expectedRowBytes * bitmap.dimensions.y);
    for (int y = 0; y < bitmap.dimensions.y; ++y) {
      std::memcpy(cache.cachedImage.data.data() + y * expectedRowBytes,
                  bitmap.pixels.data() + y * bitmap.rowBytes, expectedRowBytes);
    }
  }

  cache.imageDirty = false;
}

void CompositedRenderer::composeLayers() {
  // Begin a frame on the main renderer.
  RenderViewport viewport;
  viewport.size = Vector2d(canvasSize_.x, canvasSize_.y);
  viewport.devicePixelRatio = 1.0;
  renderer_.beginFrame(viewport);

  // Draw each layer's cached pixmap in paint order.
  for (size_t i = 0; i < layers_.layers.size(); ++i) {
    auto& cache = layerCaches_[i];
    if (cache.bitmap.empty()) {
      continue;
    }

    // Rebuild ImageResource only when the bitmap changed since last composition.
    if (cache.imageDirty) {
      rebuildCachedImage(cache);
    }

    // Position the layer pixmap at its bounds origin, then apply any composition transform
    // (e.g., drag offset for interactive editing).
    const Boxd& layerBounds = layers_.layers[i].bounds;
    renderer_.setTransform(layers_.layers[i].compositionTransform);

    ImageParams params;
    params.targetRect = layerBounds;
    params.opacity = layers_.layers[i].compositionOpacity;

    renderer_.drawImage(cache.cachedImage, params);
  }

  renderer_.endFrame();
}

}  // namespace donner::svg
