#include "donner/svg/components/ReferenceInvalidation.h"

#include <cstddef>
#include <optional>
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
 * write, or any restyle other than a compositing one. A restyle of an ancestor also counts: it can
 * change the font size that `em` geometry resolves against, or make a selector rule set the path's
 * own `transform` or `d`.
 */
bool PathGeometryMayHaveChanged(uint16_t flags) {
  constexpr uint16_t kGeometryWrites =
      DirtyFlagsComponent::Shape | DirtyFlagsComponent::Layout | DirtyFlagsComponent::Transform;
  return (flags & kGeometryWrites) != 0 || ((flags & DirtyFlagsComponent::Style) != 0 &&
                                            (flags & DirtyFlagsComponent::Compositing) == 0);
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

/// An `feImage` fragment reference: the `<filter>` holding the primitive and the element it draws.
struct FeImageReference {
  Entity filter;  //!< The `<filter>` that contains the `feImage`.
  Entity target;  //!< The element the fragment reference resolves to.
};

/// Resolve every same-document `feImage` fragment reference once.
std::vector<FeImageReference> FeImageReferences(Registry& registry) {
  std::vector<FeImageReference> references;
  for (const auto& [entity, image] : registry.view<const FEImageComponent>().each()) {
    const Entity target = ResolveFragment(registry, image.href);
    const Entity filter = target != entt::null ? OwningFilter(registry, entity) : entt::null;
    if (filter != entt::null) {
      references.push_back({filter, target});
    }
  }
  return references;
}

/// The filters of \p references whose target is in \p subtreeRoots, leaving out those already in
/// \p visited (and adding the rest).
std::vector<Entity> FiltersDrawingChanges(const std::vector<FeImageReference>& references,
                                          const std::unordered_set<Entity>& subtreeRoots,
                                          std::unordered_set<Entity>& visited) {
  std::vector<Entity> filters;
  for (const FeImageReference& reference : references) {
    if (subtreeRoots.contains(reference.target) && visited.insert(reference.filter).second) {
      filters.push_back(reference.filter);
    }
  }
  return filters;
}

/// Add to \p filters the filters that inherit their primitives from one of them through `href`,
/// breadth first; \p visited stops cycles.
void AddInheritingFilters(const std::unordered_map<Entity, std::vector<Entity>>& inheritors,
                          std::unordered_set<Entity>& visited, std::vector<Entity>& filters) {
  for (std::size_t i = 0; i < filters.size(); ++i) {
    const auto it = inheritors.find(filters[i]);
    if (it == inheritors.end()) {
      continue;
    }
    for (const Entity inheritor : it->second) {
      if (visited.insert(inheritor).second) {
        filters.push_back(inheritor);
      }
    }
  }
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
 * filter is reached; each round reaches at least one new filter, which bounds it. The `href`
 * inheritors are only looked up once some filter is reached.
 */
void InvalidateFeImageHosts(Registry& registry) {
  if (registry.view<const FEImageComponent>().empty()) {
    return;
  }
  const std::vector<FeImageReference> references = FeImageReferences(registry);
  std::unordered_set<Entity> subtreeRoots;
  for (const Entity entity : registry.view<const DirtyFlagsComponent>()) {
    AddWithAncestors(registry, entity, subtreeRoots);
  }
  std::optional<std::unordered_map<Entity, std::vector<Entity>>> inheritors;
  std::unordered_set<Entity> visited;
  for (std::vector<Entity> reached = FiltersDrawingChanges(references, subtreeRoots, visited);
       !reached.empty(); reached = FiltersDrawingChanges(references, subtreeRoots, visited)) {
    if (!inheritors.has_value()) {
      inheritors = FilterInheritors(registry);
    }
    AddInheritingFilters(*inheritors, visited, reached);
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
