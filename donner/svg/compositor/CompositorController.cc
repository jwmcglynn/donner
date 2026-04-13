#include "donner/svg/compositor/CompositorController.h"

#include <algorithm>
#include <cmath>

#include "donner/base/Utils.h"
#include "donner/svg/compositor/LayerMembershipComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg::compositor {

CompositorController::CompositorController(SVGDocument& document, RendererInterface& renderer)
    : document_(&document), renderer_(&renderer) {}

CompositorController::~CompositorController() = default;

CompositorController::CompositorController(CompositorController&&) noexcept = default;
CompositorController& CompositorController::operator=(CompositorController&&) noexcept = default;

bool CompositorController::promoteEntity(Entity entity) {
  Registry& registry = document_->registry();

  if (!registry.valid(entity)) {
    return false;
  }

  // Already promoted.
  if (registry.all_of<LayerMembershipComponent>(entity)) {
    return true;
  }

  if (static_cast<int>(layers_.size()) >= kMaxCompositorLayers) {
    return false;
  }

  if (totalBitmapMemory() >= kMaxCompositorMemoryBytes) {
    return false;
  }

  auto [firstEntity, lastEntity] = computeEntityRange(registry, entity);

  const uint32_t layerId = nextLayerId_++;
  layers_.emplace_back(layerId, entity, firstEntity, lastEntity);

  // Detect compositing features that require conservative fallback.
  if (registry.all_of<components::RenderingInstanceComponent>(entity)) {
    const auto& instance = registry.get<components::RenderingInstanceComponent>(entity);
    layers_.back().setFallbackReasons(detectFallbackReasons(instance));
  }

  registry.emplace<LayerMembershipComponent>(entity, LayerMembershipComponent{layerId});

  rootDirty_ = true;
  return true;
}

void CompositorController::demoteEntity(Entity entity) {
  Registry& registry = document_->registry();

  if (registry.valid(entity) && registry.all_of<LayerMembershipComponent>(entity)) {
    registry.remove<LayerMembershipComponent>(entity);
  }

  auto it = std::find_if(layers_.begin(), layers_.end(), [entity](const CompositorLayer& layer) {
    return layer.entity() == entity;
  });
  if (it != layers_.end()) {
    layers_.erase(it);
  }

  rootDirty_ = true;
}

bool CompositorController::isPromoted(Entity entity) const {
  return findLayer(entity) != nullptr;
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

void CompositorController::renderFrame(const RenderViewport& viewport) {
  UTILS_RELEASE_ASSERT(document_ != nullptr);
  UTILS_RELEASE_ASSERT(renderer_ != nullptr);

  // If no promoted layers, take the simple full-render path.
  if (layers_.empty()) {
    RendererDriver driver(*renderer_);
    driver.draw(*document_);
    rootDirty_ = false;
    return;
  }

  // Prepare the document (styles, layout, render tree).
  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(*document_, /*verbose=*/false, warningSink);

  // Check dirty flags on promoted entities.
  consumeDirtyFlags();

  // Re-rasterize dirty promoted layers. Layers requiring conservative fallback are always dirty.
  for (auto& layer : layers_) {
    if (layer.requiresConservativeFallback() || layer.isDirty() || !layer.hasValidBitmap()) {
      rasterizeLayer(layer, viewport);
    }
  }

  // Re-rasterize root layer if dirty.
  if (rootDirty_ || rootBitmap_.empty()) {
    rasterizeRootLayer(viewport);
  }

  // Compose all layers onto the main target.
  composeLayers(viewport);
}

size_t CompositorController::layerCount() const {
  return layers_.size();
}

size_t CompositorController::totalBitmapMemory() const {
  size_t total = rootBitmap_.pixels.size();
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
  if (!offscreen) {
    // Backend doesn't support offscreen rendering. Fall back: rasterize via the main renderer
    // using drawEntityRange, which produces the layer bitmap but ties up the main target.
    layer.clearDirty();
    return;
  }

  Registry& registry = document_->registry();

  RendererDriver driver(*offscreen);
  driver.drawEntityRange(registry, layer.firstEntity(), layer.lastEntity(), viewport,
                         Transform2d());

  layer.setBitmap(offscreen->takeSnapshot());
}

void CompositorController::rasterizeRootLayer(const RenderViewport& viewport) {
  // V1 approach: render the full document into the root layer bitmap.
  // Promoted entities are included in the root (overdrawn by promoted layer bitmaps during
  // composition). This is correct because promoted layers are composited ON TOP with their
  // composition transforms; the root copy at the original position is hidden under the
  // promoted layer's bitmap at the same position (identity composition transform means exact
  // overlap, translation means the promoted layer moves and the root shows the "hole").
  //
  // For correct hole-punching, v2 will skip promoted entity ranges from the root. For v1,
  // the overlapping approach works when the promoted layer's opacity is 1.0.
  auto offscreen = renderer_->createOffscreenInstance();
  if (offscreen) {
    RendererDriver driver(*offscreen);
    driver.draw(*document_);
    rootBitmap_ = offscreen->takeSnapshot();
  } else {
    // No offscreen support — render directly and snapshot.
    RendererDriver driver(*renderer_);
    driver.draw(*document_);
    rootBitmap_ = renderer_->takeSnapshot();
  }

  rootDirty_ = false;
}

void CompositorController::composeLayers(const RenderViewport& viewport) {
  renderer_->beginFrame(viewport);

  // Blit the root layer bitmap.
  if (!rootBitmap_.empty()) {
    ImageResource rootImage;
    rootImage.data = rootBitmap_.pixels;
    rootImage.width = rootBitmap_.dimensions.x;
    rootImage.height = rootBitmap_.dimensions.y;

    ImageParams params;
    params.targetRect =
        Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(rootBitmap_.dimensions.x),
                                         static_cast<double>(rootBitmap_.dimensions.y)));

    renderer_->setTransform(Transform2d());
    renderer_->drawImage(rootImage, params);
  }

  // Blit each promoted layer with its composition transform.
  for (const auto& layer : layers_) {
    if (!layer.hasValidBitmap()) {
      continue;
    }

    const RendererBitmap& bitmap = layer.bitmap();
    ImageResource layerImage;
    layerImage.data = bitmap.pixels;
    layerImage.width = bitmap.dimensions.x;
    layerImage.height = bitmap.dimensions.y;

    ImageParams params;
    params.targetRect = Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(bitmap.dimensions.x),
                                                         static_cast<double>(bitmap.dimensions.y)));

    // Apply composition transform with integer-pixel snap for translations.
    Transform2d compositionTransform = layer.compositionTransform();
    if (compositionTransform.isTranslation()) {
      Vector2d t = compositionTransform.translation();
      compositionTransform = Transform2d::Translate(std::round(t.x), std::round(t.y));
    }

    renderer_->setTransform(compositionTransform);
    renderer_->drawImage(layerImage, params);
  }

  renderer_->endFrame();
}

void CompositorController::consumeDirtyFlags() {
  Registry& registry = document_->registry();

  auto view = registry.view<components::DirtyFlagsComponent>();
  for (auto& layer : layers_) {
    if (!registry.valid(layer.entity())) {
      layer.markDirty();
      continue;
    }

    if (view.contains(layer.entity())) {
      const auto& dirty = view.get<components::DirtyFlagsComponent>(layer.entity());
      if (dirty.flags != components::DirtyFlagsComponent::Flags::None) {
        layer.markDirty();
        rootDirty_ = true;
      }
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
