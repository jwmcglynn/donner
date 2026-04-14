#include "donner/svg/compositor/CompositorController.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/svg/compositor/LayerMembershipComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg::compositor {

namespace {

struct HiddenVisibilityState {
  Entity entity;
  bool visible;
};

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

    const auto alreadyHidden =
        std::find_if(hiddenEntities->begin(), hiddenEntities->end(),
                     [entity](const HiddenVisibilityState& state) { return state.entity == entity; });
    if (alreadyHidden == hiddenEntities->end()) {
      hiddenEntities->push_back(HiddenVisibilityState{.entity = entity, .visible = instance.visible});
    }

    instance.visible = false;

    if (entity == layer.lastEntity()) {
      break;
    }

    view.advance();
  }
}

void RestoreLayerVisibility(Registry& registry,
                            const std::vector<HiddenVisibilityState>& hiddenEntities) {
  for (const auto& hidden : hiddenEntities) {
    if (registry.valid(hidden.entity)) {
      registry.get<components::RenderingInstanceComponent>(hidden.entity).visible = hidden.visible;
    }
  }
}

}  // namespace

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

  Registry& registry = document_->registry();
  if (registry.view<components::DirtyFlagsComponent>().begin() !=
      registry.view<components::DirtyFlagsComponent>().end()) {
    rootDirty_ = true;
  }
  if (registry.ctx().contains<components::RenderTreeState>() &&
      registry.ctx().get<components::RenderTreeState>().needsFullRebuild) {
    rootDirty_ = true;
  }

  // If no promoted layers, take the simple full-render path.
  if (layers_.empty()) {
    RendererDriver driver(*renderer_);
    driver.draw(*document_);
    rootDirty_ = false;
    documentPrepared_ = true;
    return;
  }

  // Only prepare the document when it hasn't been prepared yet or when dirty.
  // prepareDocumentForRendering rebuilds the render instance tree, which is expensive.
  // During drag (composition transform changes only), we skip preparation since the
  // document content hasn't changed.
  if (!documentPrepared_ || rootDirty_) {
    ParseWarningSink warningSink;
    RendererUtils::prepareDocumentForRendering(*document_, /*verbose=*/false, warningSink);
    documentPrepared_ = true;
    refreshLayerMetadata();

    // After preparation, clear the needsFullRebuild flag so consumeDirtyFlags doesn't
    // re-trigger a full rebuild on the next frame. The render tree instantiation process
    // leaves needsFullRebuild=true as a side effect of invalidateRenderTree(); we consume
    // that signal here.
    if (document_->registry().ctx().contains<components::RenderTreeState>()) {
      document_->registry().ctx().get<components::RenderTreeState>().needsFullRebuild = false;
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
  UTILS_RELEASE_ASSERT(offscreen != nullptr);

  Registry& registry = document_->registry();

  RendererDriver driver(*offscreen);
  driver.drawEntityRange(registry, layer.firstEntity(), layer.lastEntity(), viewport,
                         Transform2d());

  layer.setBitmap(offscreen->takeSnapshot());
}

void CompositorController::rasterizeRootLayer(const RenderViewport& viewport) {
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

  auto dirtyView = registry.view<components::DirtyFlagsComponent>();
  for (const Entity entity : dirtyView) {
    const auto& dirty = dirtyView.get<components::DirtyFlagsComponent>(entity);
    if (dirty.flags == components::DirtyFlagsComponent::Flags::None) {
      continue;
    }

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
