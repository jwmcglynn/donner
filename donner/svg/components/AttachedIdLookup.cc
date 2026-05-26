#include "donner/svg/components/AttachedIdLookup.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg::components {
namespace {

Entity FindAttachedEntityByIdInSubtree(Registry& registry, Entity root, const RcString& id) {
  if (root == entt::null || !registry.valid(root)) {
    return entt::null;
  }

  if (const auto* idComponent = registry.try_get<IdComponent>(root);
      idComponent != nullptr && idComponent->id() == id &&
      IsEntityAttachedToDocument(registry, root)) {
    return root;
  }

  const auto* tree = registry.try_get<donner::components::TreeComponent>(root);
  if (tree == nullptr) {
    return entt::null;
  }

  for (Entity child = tree->firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    const Entity match = FindAttachedEntityByIdInSubtree(registry, child, id);
    if (match != entt::null) {
      return match;
    }
  }

  return entt::null;
}

}  // namespace

Entity FindAttachedEntityById(Registry& registry, const RcString& id) {
  const auto* context = registry.ctx().find<const SVGDocumentContext>();
  if (context == nullptr) {
    return entt::null;
  }

  const Entity mappedEntity = context->getEntityById(id);
  if (IsEntityAttachedToDocument(registry, mappedEntity)) {
    return mappedEntity;
  }

  return FindAttachedEntityByIdInSubtree(registry, context->rootEntity, id);
}

}  // namespace donner::svg::components
