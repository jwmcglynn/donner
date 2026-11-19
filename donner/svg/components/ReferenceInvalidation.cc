#include "donner/svg/components/ReferenceInvalidation.h"

#include <unordered_set>
#include <variant>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {
namespace {

/// The flags a transform change of an ancestor leaves on its descendants. They do not change the
/// geometry of the element itself in its parent's coordinates.
constexpr uint16_t kInheritedOnlyFlags =
    DirtyFlagsComponent::WorldTransform | DirtyFlagsComponent::RenderInstance;

/// Resolve a same-document fragment reference such as `#id`; external references resolve to null.
Entity ResolveFragment(Registry& registry, const RcString& href) {
  const Reference reference(href);
  if (reference.isExternal()) {
    return entt::null;
  }
  const std::optional<ResolvedReference> resolved = reference.resolve(registry);
  return resolved.has_value() ? resolved->handle.entity() : entt::null;
}

Entity ParentOf(const Registry& registry, Entity entity) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
  return tree != nullptr ? tree->parent() : entt::null;
}

/// A text path follows its path's geometry and the path's own transform, which the path changes
/// itself. Its ancestors' transforms do not apply.
void InvalidateTextPathLayouts(Registry& registry) {
  for (const auto& [entity, textPath] : registry.view<const TextPathComponent>().each()) {
    if ((textPath.inlinePath.has_value() && !textPath.inlinePath->empty()) ||
        textPath.href.empty()) {
      continue;
    }
    const Entity path = ResolveFragment(registry, textPath.href);
    const auto* dirty = path != entt::null ? registry.try_get<DirtyFlagsComponent>(path) : nullptr;
    if (dirty != nullptr && (dirty->flags & ~kInheritedOnlyFlags) != 0) {
      (void)InvalidateTextLayout(EntityHandle(registry, entity));
    }
  }
}

/// Every dirty entity and its ancestors: an `feImage` that references any of them draws a change.
std::unordered_set<Entity> CollectChangedSubtreeRoots(const Registry& registry) {
  std::unordered_set<Entity> result;
  for (const Entity entity : registry.view<const DirtyFlagsComponent>()) {
    // An ancestor already in the set had its own ancestors added with it.
    Entity current = entity;
    while (current != entt::null && result.insert(current).second) {
      current = ParentOf(registry, current);
    }
  }
  return result;
}

/// The `<filter>` that contains \p primitive, or null.
Entity OwningFilter(const Registry& registry, Entity primitive) {
  for (Entity current = ParentOf(registry, primitive); current != entt::null;
       current = ParentOf(registry, current)) {
    const auto* type = registry.try_get<ElementTypeComponent>(current);
    if (type != nullptr && type->type() == ElementType::Filter) {
      return current;
    }
  }
  return entt::null;
}

/// Add the filters that take their primitives from one already in \p filters through `href`.
void AddInheritingFilters(Registry& registry, std::unordered_set<Entity>& filters) {
  for (bool added = true; added;) {
    added = false;
    for (const auto& [entity, filter] : registry.view<const FilterComponent>().each()) {
      if (filter.href.has_value() && !filters.contains(entity)) {
        const std::optional<ResolvedReference> resolved = filter.href->resolve(registry);
        if (resolved.has_value() && filters.contains(resolved->handle.entity())) {
          filters.insert(entity);
          added = true;
        }
      }
    }
  }
}

/// The `<filter>` elements holding an `feImage` that references an element in \p changed.
std::unordered_set<Entity> FiltersDrawingChanges(Registry& registry,
                                                 const std::unordered_set<Entity>& changed) {
  std::unordered_set<Entity> filters;
  for (const auto& [entity, image] : registry.view<const FEImageComponent>().each()) {
    const Entity target = ResolveFragment(registry, image.href);
    if (target == entt::null || !changed.contains(target)) {
      continue;
    }
    if (const Entity filter = OwningFilter(registry, entity); filter != entt::null) {
      filters.insert(filter);
    }
  }
  return filters;
}

/// The rendering instances whose filter is one of \p filters.
std::vector<Entity> FilterHosts(const Registry& registry,
                                const std::unordered_set<Entity>& filters) {
  std::vector<Entity> hosts;
  for (const auto& [entity, instance] : registry.view<const RenderingInstanceComponent>().each()) {
    const auto* reference = instance.resolvedFilter.has_value()
                                ? std::get_if<ResolvedReference>(&instance.resolvedFilter.value())
                                : nullptr;
    if (reference != nullptr && filters.contains(reference->handle.entity())) {
      hosts.push_back(entity);
    }
  }
  return hosts;
}

/// An element whose filter draws a changed element through `feImage` redraws its filter result.
void InvalidateFeImageHosts(Registry& registry) {
  if (registry.view<const FEImageComponent>().empty()) {
    return;
  }
  std::unordered_set<Entity> filters =
      FiltersDrawingChanges(registry, CollectChangedSubtreeRoots(registry));
  if (filters.empty()) {
    return;
  }
  AddInheritingFilters(registry, filters);
  for (const Entity host : FilterHosts(registry, filters)) {
    registry.get_or_emplace<DirtyFlagsComponent>(host).mark(DirtyFlagsComponent::Filter |
                                                            DirtyFlagsComponent::RenderInstance);
  }
}

}  // namespace

void InvalidateReferenceDependents(Registry& registry) {
  if (registry.view<const DirtyFlagsComponent>().empty()) {
    return;
  }
  InvalidateTextPathLayouts(registry);
  InvalidateFeImageHosts(registry);
}

}  // namespace donner::svg::components
