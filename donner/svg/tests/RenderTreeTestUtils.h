#pragma once
/// @file
///
/// Test-only helpers for inspecting render-tree invalidation state. These wrap
/// ECS-internal components (DirtyFlagsComponent, RenderingInstanceComponent)
/// so tests outside donner/svg - e.g. editor Layers-panel tests - can assert
/// render-invalidation behavior without including component headers directly
/// (which the banned-patterns lint forbids).

#include "donner/base/EcsRegistry.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg::tests {

/// Count prepared rendering instances whose data entity matches \p dataEntity.
inline int CountRenderingInstancesForDataEntity(const SVGDocument& document, Entity dataEntity) {
  int count = 0;
  const Registry& registry = document.registry();
  for (auto view = registry.view<const components::RenderingInstanceComponent>();
       const Entity storageEntity : view) {
    const auto& instance = view.get<const components::RenderingInstanceComponent>(storageEntity);
    if (instance.dataEntity == dataEntity) {
      ++count;
    }
  }
  return count;
}

/// Count all prepared rendering instances in the document's render tree.
inline int CountRenderingInstances(const SVGDocument& document) {
  int count = 0;
  for (auto view = document.registry().view<const components::RenderingInstanceComponent>();
       const Entity storageEntity : view) {
    (void)storageEntity;
    ++count;
  }
  return count;
}

/// True when \p element carries a pending RenderInstance dirty flag.
inline bool ElementHasRenderInstanceDirtyFlag(const svg::SVGElement& element) {
  const auto* dirty = element.entityHandle().try_get<components::DirtyFlagsComponent>();
  return dirty != nullptr && dirty->test(components::DirtyFlagsComponent::RenderInstance);
}

/// True when the document has a queued full style recompute.
inline bool DocumentNeedsFullStyleRecompute(const SVGDocument& document) {
  const auto* state = document.registry().ctx().find<components::RenderTreeState>();
  return state != nullptr && state->needsFullStyleRecompute;
}

}  // namespace donner::svg::tests
