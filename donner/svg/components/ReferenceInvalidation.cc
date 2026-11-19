#include "donner/svg/components/ReferenceInvalidation.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {
namespace {

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

/**
 * Whether \p flags may change a path's geometry or its own transform: a shape, layout or transform
 * write, or a restyle of the path itself, which marks the whole \ref
 * DirtyFlagsComponent::StyleCascade. Style inherited from a restyled ancestor (Style and Paint
 * only) and compositing changes cannot change geometry.
 */
bool PathGeometryMayHaveChanged(uint16_t flags) {
  constexpr uint16_t kGeometryWrites =
      DirtyFlagsComponent::Shape | DirtyFlagsComponent::Layout | DirtyFlagsComponent::Transform;
  return (flags & kGeometryWrites) != 0 ||
         (flags & DirtyFlagsComponent::StyleCascade) == DirtyFlagsComponent::StyleCascade;
}

/// A text path follows its path's geometry and the path's own transform. Its ancestors'
/// transforms do not apply.
void InvalidateTextPathLayouts(Registry& registry) {
  for (const auto& [entity, textPath] : registry.view<const TextPathComponent>().each()) {
    if ((textPath.inlinePath.has_value() && !textPath.inlinePath->empty()) ||
        textPath.href.empty()) {
      continue;
    }
    const Entity path = ResolveFragment(registry, textPath.href);
    const auto* dirty = path != entt::null ? registry.try_get<DirtyFlagsComponent>(path) : nullptr;
    if (dirty != nullptr && PathGeometryMayHaveChanged(dirty->flags)) {
      (void)InvalidateTextLayout(EntityHandle(registry, entity));
    }
  }
}

/// Add \p entity and its ancestors to \p subtreeRoots: an `feImage` that references any of them
/// draws \p entity.
void AddWithAncestors(const Registry& registry, Entity entity,
                      std::unordered_set<Entity>& subtreeRoots) {
  // An ancestor already in the set had its own ancestors added with it.
  while (entity != entt::null && subtreeRoots.insert(entity).second) {
    entity = ParentOf(registry, entity);
  }
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

/// For each `<filter>`, the filters that take their primitives from it through `href`.
std::unordered_map<Entity, std::vector<Entity>> FilterInheritors(Registry& registry) {
  std::unordered_map<Entity, std::vector<Entity>> inheritors;
  for (const auto& [entity, filter] : registry.view<const FilterComponent>().each()) {
    if (!filter.href.has_value()) {
      continue;
    }
    if (const std::optional<ResolvedReference> resolved = filter.href->resolve(registry)) {
      inheritors[resolved->handle.entity()].push_back(entity);
    }
  }
  return inheritors;
}

/// The filters holding an `feImage` that references an entity in \p subtreeRoots, and the filters
/// that inherit their primitives, leaving out those already in \p visited (and adding the rest).
std::vector<Entity> NewFiltersDrawing(
    Registry& registry, const std::unordered_set<Entity>& subtreeRoots,
    const std::unordered_map<Entity, std::vector<Entity>>& inheritors,
    std::unordered_set<Entity>& visited) {
  std::deque<Entity> pending;
  for (const auto& [entity, image] : registry.view<const FEImageComponent>().each()) {
    const Entity target = ResolveFragment(registry, image.href);
    if (target == entt::null || !subtreeRoots.contains(target)) {
      continue;
    }
    if (const Entity filter = OwningFilter(registry, entity);
        filter != entt::null && visited.insert(filter).second) {
      pending.push_back(filter);
    }
  }
  // Breadth first over the href inheritors; `visited` stops cycles.
  std::vector<Entity> result;
  while (!pending.empty()) {
    const Entity filter = pending.front();
    pending.pop_front();
    result.push_back(filter);
    if (const auto it = inheritors.find(filter); it != inheritors.end()) {
      for (const Entity inheritor : it->second) {
        if (visited.insert(inheritor).second) {
          pending.push_back(inheritor);
        }
      }
    }
  }
  return result;
}

/// Whether the resolved filter of \p instance draws one of \p filters, either as its single
/// `url()` or as one `url()` stage of a filter list.
bool DrawsAnyFilter(Registry& registry, const RenderingInstanceComponent& instance,
                    const std::unordered_set<Entity>& filters) {
  if (!instance.resolvedFilter.has_value()) {
    return false;
  }
  if (const auto* reference = std::get_if<ResolvedReference>(&*instance.resolvedFilter)) {
    return filters.contains(reference->handle.entity());
  }
  for (const FilterEffect& effect : std::get<std::vector<FilterEffect>>(*instance.resolvedFilter)) {
    if (!effect.is<FilterEffect::ElementReference>()) {
      continue;
    }
    const std::optional<ResolvedReference> resolved =
        effect.get<FilterEffect::ElementReference>().reference.resolve(registry);
    if (resolved.has_value() && filters.contains(resolved->handle.entity())) {
      return true;
    }
  }
  return false;
}

/// The rendering instances whose filter draws one of \p filters.
std::vector<Entity> FilterHosts(Registry& registry, const std::unordered_set<Entity>& filters) {
  std::vector<Entity> hosts;
  for (const auto& [entity, instance] : registry.view<const RenderingInstanceComponent>().each()) {
    if (DrawsAnyFilter(registry, instance, filters)) {
      hosts.push_back(entity);
    }
  }
  return hosts;
}

/**
 * An element whose filter draws a changed element through `feImage` redraws its filter result.
 * A redrawn host is itself a change that another `feImage` may draw, so this repeats until no new
 * filter is reached; each round reaches at least one new filter, which bounds it.
 */
void InvalidateFeImageHosts(Registry& registry) {
  if (registry.view<const FEImageComponent>().empty()) {
    return;
  }
  std::unordered_set<Entity> subtreeRoots;
  for (const Entity entity : registry.view<const DirtyFlagsComponent>()) {
    AddWithAncestors(registry, entity, subtreeRoots);
  }
  const auto inheritors = FilterInheritors(registry);
  std::unordered_set<Entity> visited;
  for (std::vector<Entity> reached = NewFiltersDrawing(registry, subtreeRoots, inheritors, visited);
       !reached.empty(); reached = NewFiltersDrawing(registry, subtreeRoots, inheritors, visited)) {
    const std::unordered_set<Entity> filters(reached.begin(), reached.end());
    for (const Entity host : FilterHosts(registry, filters)) {
      registry.get_or_emplace<DirtyFlagsComponent>(host).mark(
          DirtyFlagsComponent::ReferencedContent | DirtyFlagsComponent::RenderInstance);
      AddWithAncestors(registry, host, subtreeRoots);
    }
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
