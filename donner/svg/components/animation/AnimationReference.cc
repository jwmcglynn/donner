#include "donner/svg/components/animation/AnimationReference.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg::components {
namespace {

Entity NextReferenceNode(Registry& registry, Entity current, Entity root,
                         AnimationResourceBudget& budget) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
  if (tree && tree->firstChild() != entt::null) {
    return tree->firstChild();
  }
  while (tree && current != root && budget.reserveReferenceWork(1)) {
    if (tree->nextSibling() != entt::null) {
      return tree->nextSibling();
    }
    current = tree->parent();
    tree = registry.try_get<donner::components::TreeComponent>(current);
  }
  return entt::null;
}

Entity FindInDocument(Registry& registry, Entity root, std::string_view id,
                      AnimationResourceBudget& budget) {
  Entity current = root;
  while (current != entt::null && budget.reserveReferenceWork(1)) {
    if (!registry.valid(current)) {
      return entt::null;
    }
    const auto* candidate = registry.try_get<IdComponent>(current);
    if (candidate && candidate->id().size() == id.size() &&
        IsEntityAttachedToDocument(registry, current)) {
      if (!budget.reserveReferenceWork(id.size())) {
        return entt::null;
      }
      if (std::string_view(candidate->id()) == id) {
        return current;
      }
    }
    current = NextReferenceNode(registry, current, root, budget);
  }
  return entt::null;
}

}  // namespace

Entity FindAnimationReference(Registry& registry, std::string_view id,
                              AnimationResourceBudget& budget) {
  if (!budget.reserveReferenceWork(id.size()) || !budget.reserveReferenceWork(id.size()) ||
      !budget.reserveReferenceWork(1)) {
    return entt::null;
  }
  const auto& context = registry.ctx().get<SVGDocumentContext>();
  const Entity mapped = context.getEntityById(RcString(id));
  if (IsEntityAttachedToDocument(registry, mapped)) {
    return mapped;
  }
  return FindInDocument(registry, context.rootEntity, id, budget);
}

}  // namespace donner::svg::components
