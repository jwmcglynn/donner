#include "donner/svg/components/text/TextInvalidation.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
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

}  // namespace donner::svg::components
