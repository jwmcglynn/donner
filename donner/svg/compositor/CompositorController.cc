#include "donner/svg/compositor/CompositorController.h"

#include <algorithm>

#include "donner/base/Utils.h"
#include "donner/svg/compositor/LayerMembershipComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/renderer/RendererDriver.h"

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

void CompositorController::renderFrame(const RenderViewport& viewport) {
  UTILS_RELEASE_ASSERT(document_ != nullptr);
  UTILS_RELEASE_ASSERT(renderer_ != nullptr);

  // Skeleton implementation: full re-render every frame via RendererDriver.
  // The compositing fast path (layer caching + bitmap composition) is added in a later commit.
  RendererDriver driver(*renderer_);
  driver.draw(*document_);

  rootDirty_ = false;
  for (auto& layer : layers_) {
    layer.clearDirty();
  }
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

}  // namespace donner::svg::compositor
