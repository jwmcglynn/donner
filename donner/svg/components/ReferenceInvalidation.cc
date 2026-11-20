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
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {
namespace {

/// The flags that mark an element as redrawn for content it draws through a reference.
constexpr uint16_t kReferencedContentFlags =
    DirtyFlagsComponent::ReferencedContent | DirtyFlagsComponent::RenderInstance;

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

bool IsElementType(const Registry& registry, Entity entity, ElementType type) {
  const auto* component = registry.try_get<ElementTypeComponent>(entity);
  return component != nullptr && component->type() == type;
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

/// Whether \p flags only carry a transform inherited from a moved ancestor. A `<use>` copy of the
/// element is placed by the `<use>`, not by the element's ancestors, so it does not move.
bool OnlyAncestorTransform(uint16_t flags) {
  constexpr uint16_t kInheritedTransform =
      DirtyFlagsComponent::WorldTransform | DirtyFlagsComponent::RenderInstance;
  return (flags & DirtyFlagsComponent::WorldTransform) != 0 && (flags & ~kInheritedTransform) == 0;
}

/// Whether a `<use>` lies inside a `<clipPath>` or `<mask>`, so a clip or mask may draw a copy.
bool ClipOrMaskHoldsUse(Registry& registry) {
  bool found = false;
  const auto searchContent = [&](Entity resource) {
    donner::components::ForAllChildrenRecursivePruned(
        EntityHandle(registry, resource), [&](EntityHandle descendant) {
          found = found || IsElementType(registry, descendant.entity(), ElementType::Use);
          return !found;
        });
  };
  for (const Entity clipPath : registry.view<const ClipPathComponent>()) {
    searchContent(clipPath);
    if (found) {
      return true;
    }
  }
  for (const Entity mask : registry.view<const MaskComponent>()) {
    searchContent(mask);
    if (found) {
      return true;
    }
  }
  return false;
}

/**
 * The `<use>` copies a clip path or mask can draw: for each element, the `<use>` elements that copy
 * it, or nothing when no `<use>` lies inside a `<clipPath>` or `<mask>`. Kept in the registry
 * context and reused until the document is rebuilt as a whole. Every edit that can change it (a
 * tree mutation, an `id` change, an in-document `href` retarget) takes that rebuild.
 */
struct UseCopyRelation {
  uint64_t fullRebuilds = 0;  //!< \ref RenderTreeState::Counters::fullRebuilds when built.
  std::unordered_map<Entity, std::vector<Entity>> copies;  //!< Target to its `<use>` copies.
};

/// Build \ref UseCopyRelation::copies for the current document.
std::unordered_map<Entity, std::vector<Entity>> UseCopies(Registry& registry) {
  std::unordered_map<Entity, std::vector<Entity>> copies;
  if (!ClipOrMaskHoldsUse(registry)) {
    return copies;
  }
  for (const auto& [entity, shadowTree] : registry.view<const ShadowTreeComponent>().each()) {
    if (!IsElementType(registry, entity, ElementType::Use)) {
      continue;
    }
    const std::optional<RcString> href = shadowTree.mainHref();
    const Entity target = href.has_value() ? ResolveFragment(registry, *href) : entt::null;
    if (target != entt::null) {
      copies[target].push_back(entity);
    }
  }
  return copies;
}

/**
 * The \ref UseCopyRelation for this frame. It is rebuilt when the document was rebuilt as a whole
 * since it was cached, or when this frame's edits can change it: a pending whole-document rebuild
 * or restyle, or a `<use>` whose shadow tree is dirty. Those frames recompute the whole document
 * anyway, so the frames that reuse it are the ones that keep the retained update.
 */
const std::unordered_map<Entity, std::vector<Entity>>& CurrentUseCopies(Registry& registry,
                                                                        bool structureChanged) {
  auto* state = registry.ctx().find<RenderTreeState>();
  const uint64_t fullRebuilds = state != nullptr ? state->counters.fullRebuilds : 0;
  auto* cached = registry.ctx().find<UseCopyRelation>();
  if (cached != nullptr && state != nullptr && !structureChanged && !state->needsFullRebuild &&
      !state->needsFullStyleRecompute && cached->fullRebuilds == fullRebuilds) {
    return cached->copies;
  }
  if (state != nullptr) {
    ++state->counters.useCopyRelationBuilds;
  }
  return registry.ctx().insert_or_assign(UseCopyRelation{fullRebuilds, UseCopies(registry)}).copies;
}

/// For each `<clipPath>` and `<mask>`, the elements whose `clip-path` or `mask` property uses it.
/// This includes a clip path or mask that clips or masks itself, the content of one, and elements
/// in `<use>` copies.
std::unordered_map<Entity, std::vector<Entity>> ClipAndMaskUsers(Registry& registry) {
  std::unordered_map<Entity, std::vector<Entity>> users;
  const auto addUser = [&](const Reference& reference, Entity user) {
    if (const std::optional<ResolvedReference> resolved = reference.resolve(registry)) {
      users[resolved->handle.entity()].push_back(user);
    }
  };
  for (const auto& [entity, style] : registry.view<const ComputedStyleComponent>().each()) {
    if (!style.properties.has_value()) {
      continue;
    }
    if (const ClipPath* clip = style.properties->clipPath.getStoredValue()) {
      if (const auto* reference = std::get_if<Reference>(&clip->value)) {
        addUser(*reference, entity);
      }
    }
    if (const std::optional<Reference> mask = style.properties->mask.get()) {
      addUser(*mask, entity);
    }
  }
  return users;
}

/**
 * The elements whose drawing changed this frame, closed over the references that draw one element
 * as part of another: an element's ancestors draw it, a `<use>` draws a copy of its target, and an
 * element clipped or masked by a `<clipPath>` or `<mask>` draws its content. Each drawn element
 * clipped or masked by a changed clip path or mask is marked \ref
 * DirtyFlagsComponent::ReferencedContent.
 *
 * Every element enters the closure once, so building it is linear in the elements it reaches and
 * the references that reach them, and reference cycles terminate.
 */
class ChangedContent {
public:
  /**
   * Seed the closure with every element a DOM edit left dirty.
   *
   * @param registry Document registry.
   * @param hasClipOrMask Whether the document has a `<clipPath>` or `<mask>`.
   * @param tracksAncestorMoves Whether to remember the elements moved only by an ancestor, for
   *   \ref drawnWithAncestorsChanged.
   */
  ChangedContent(Registry& registry, bool hasClipOrMask, bool tracksAncestorMoves)
      : registry_(registry), hasClipOrMask_(hasClipOrMask) {
    for (const auto& [entity, dirty] : registry.view<const DirtyFlagsComponent>().each()) {
      structureChanged_ = structureChanged_ || dirty.test(DirtyFlagsComponent::ShadowTree);
    }
    for (const auto& [entity, dirty] : registry.view<const DirtyFlagsComponent>().each()) {
      if (!OnlyAncestorTransform(dirty.flags)) {
        pending_.push_back(entity);
      } else if (tracksAncestorMoves) {
        movedByAncestor_.insert(entity);
      }
    }
    drain();
  }

  /// Whether a reference that draws \p entity with its ancestors' transforms sees a change: the
  /// element, an element inside it, or one of its ancestors changed.
  bool drawnWithAncestorsChanged(Entity entity) const {
    return changed_.contains(entity) || movedByAncestor_.contains(entity);
  }

  /// Mark \p host redrawn for content it draws through a reference, and close over it.
  void markHost(Entity host) {
    registry_.get_or_emplace<DirtyFlagsComponent>(host).mark(kReferencedContentFlags);
    pending_.push_back(host);
    drain();
  }

private:
  /// Add each pending element with its ancestors, then everything that draws them.
  void drain() {
    while (!pending_.empty()) {
      Entity entity = pending_.back();
      pending_.pop_back();
      for (; entity != entt::null && changed_.insert(entity).second;
           entity = ParentOf(registry_, entity)) {
        addReferrers(entity);
      }
    }
  }

  /// Queue the `<use>` copies of \p entity and, for a clip path or mask, the elements that use it.
  void addReferrers(Entity entity) {
    // Only an element with an `id` can be a `<use>` target.
    if (hasClipOrMask_ && registry_.all_of<IdComponent>(entity)) {
      if (useCopies_ == nullptr) {
        useCopies_ = &CurrentUseCopies(registry_, structureChanged_);
      }
      appendAll(*useCopies_, entity, /*markHosts=*/false);
    }
    if (hasClipOrMask_ && (IsElementType(registry_, entity, ElementType::ClipPath) ||
                           IsElementType(registry_, entity, ElementType::Mask))) {
      if (!clipAndMaskUsers_.has_value()) {
        clipAndMaskUsers_ = ClipAndMaskUsers(registry_);
      }
      appendAll(*clipAndMaskUsers_, entity, /*markHosts=*/true);
    }
  }

  /// Queue the elements \p referrers lists for \p entity; when \p markHosts is set, the drawn ones
  /// among them are marked.
  void appendAll(const std::unordered_map<Entity, std::vector<Entity>>& referrers, Entity entity,
                 bool markHosts) {
    const auto it = referrers.find(entity);
    if (it == referrers.end()) {
      return;
    }
    for (const Entity referrer : it->second) {
      // A host that already holds a change, such as an ancestor of the changed element, is marked
      // too: redrawing its own content alone would keep the old clip or mask.
      if (markHosts && registry_.all_of<RenderingInstanceComponent>(referrer)) {
        registry_.get_or_emplace<DirtyFlagsComponent>(referrer).mark(kReferencedContentFlags);
      }
      if (!changed_.contains(referrer)) {
        pending_.push_back(referrer);
      }
    }
  }

  Registry& registry_;
  /// Whether the document has a `<clipPath>` or `<mask>`.
  const bool hasClipOrMask_;
  /// Whether an edit this frame retargets a `<use>`, so a cached \ref UseCopyRelation is stale.
  bool structureChanged_ = false;
  /// Elements whose drawing changed, with their ancestors.
  std::unordered_set<Entity> changed_;
  /// Elements dirty only for a transform inherited from a moved ancestor.
  std::unordered_set<Entity> movedByAncestor_;
  /// Elements waiting to enter \ref changed_.
  std::vector<Entity> pending_;
  /// The `<use>` copies, looked up on first use.
  const std::unordered_map<Entity, std::vector<Entity>>* useCopies_ = nullptr;
  /// Built on first use.
  std::optional<std::unordered_map<Entity, std::vector<Entity>>> clipAndMaskUsers_;
};

/// The `<filter>` that contains \p primitive, or null.
Entity OwningFilter(const Registry& registry, Entity primitive) {
  for (Entity current = ParentOf(registry, primitive); current != entt::null;
       current = ParentOf(registry, current)) {
    if (IsElementType(registry, current, ElementType::Filter)) {
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

/// The filters of \p references whose target is drawn with a change, leaving out those already in
/// \p visited (and adding the rest).
std::vector<Entity> FiltersDrawingChanges(const std::vector<FeImageReference>& references,
                                          const ChangedContent& changes,
                                          std::unordered_set<Entity>& visited) {
  std::vector<Entity> filters;
  for (const FeImageReference& reference : references) {
    if (changes.drawnWithAncestorsChanged(reference.target) &&
        visited.insert(reference.filter).second) {
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
 * A redrawn host is itself a change that another `feImage`, `<use>`, clip path or mask may draw,
 * so this repeats until no new filter is reached; each round reaches at least one new filter,
 * which bounds it. The `href` inheritors are only looked up once some filter is reached.
 */
void InvalidateFeImageHosts(Registry& registry, ChangedContent& changes) {
  const std::vector<FeImageReference> references = FeImageReferences(registry);
  std::optional<std::unordered_map<Entity, std::vector<Entity>>> inheritors;
  std::unordered_set<Entity> visited;
  for (std::vector<Entity> reached = FiltersDrawingChanges(references, changes, visited);
       !reached.empty(); reached = FiltersDrawingChanges(references, changes, visited)) {
    if (!inheritors.has_value()) {
      inheritors = FilterInheritors(registry);
    }
    AddInheritingFilters(*inheritors, visited, reached);
    const std::unordered_set<Entity> filters(reached.begin(), reached.end());
    for (const Entity host : FilterHosts(registry, filters)) {
      changes.markHost(host);
    }
  }
}

}  // namespace

void InvalidateReferenceDependents(Registry& registry) {
  if (registry.view<const DirtyFlagsComponent>().empty()) {
    return;
  }
  InvalidateTextPathLayouts(registry);
  const bool hasFeImage = !registry.view<const FEImageComponent>().empty();
  const bool hasClipOrMask = !registry.view<const ClipPathComponent>().empty() ||
                             !registry.view<const MaskComponent>().empty();
  if (!hasFeImage && !hasClipOrMask) {
    return;
  }
  ChangedContent changes(registry, hasClipOrMask, hasFeImage);
  InvalidateFeImageHosts(registry, changes);
}

}  // namespace donner::svg::components
