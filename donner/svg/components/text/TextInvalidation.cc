#include "donner/svg/components/text/TextInvalidation.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/GeometryPreparationResourceBudget.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"

namespace donner::svg::components {

Entity InvalidateTextLayout(EntityHandle handle) {
  Registry& registry = *handle.registry();
  // Documents without any text at all are the common case; skip the ancestor walk entirely.
  if (registry.storage<TextRootComponent>().empty()) {
    return entt::null;
  }

  Entity current = handle.entity();
  while (current != entt::null) {
    if (registry.all_of<TextRootComponent>(current)) {
      registry.remove<ComputedTextGeometryComponent>(current);
      if (auto* budget = registry.ctx().find<GeometryPreparationResourceBudget>()) {
        budget->release(current);
      }
      registry.get_or_emplace<DirtyFlagsComponent>(current).mark(
          DirtyFlagsComponent::TextGeometry | DirtyFlagsComponent::RenderInstance);
      return current;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }

    current = tree->parent();
  }

  return entt::null;
}

void InvalidateAnimatedText(Registry& registry, Entity entity) {
  if (registry.any_of<TextComponent, TextPositioningComponent, TextPathComponent>(entity)) {
    const Entity root = InvalidateTextLayout(EntityHandle(registry, entity));
    if (root != entt::null) {
      registry.remove<ComputedTextComponent>(root);
    }
  }
}

}  // namespace donner::svg::components
