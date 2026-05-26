#include "donner/svg/components/TreeMutation.h"

#include <optional>

#include "donner/base/Utils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/NodeLifetimeCollector.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg::components {
namespace {

void CheckValid(EntityHandle handle, const char* name) {
  UTILS_RELEASE_ASSERT_MSG(handle, name);
  UTILS_RELEASE_ASSERT_MSG(handle.valid(), name);
  UTILS_RELEASE_ASSERT_MSG(handle.all_of<donner::components::TreeComponent>(), name);
}

void CheckSameRegistry(EntityHandle lhs, EntityHandle rhs) {
  UTILS_RELEASE_ASSERT_MSG(lhs.registry() == rhs.registry(),
                           "TreeMutation entities must belong to the same Registry");
}

Entity ParentOf(EntityHandle handle) {
  return handle.get<donner::components::TreeComponent>().parent();
}

bool IsAncestorOf(EntityHandle ancestor, EntityHandle node) {
  Registry& registry = *ancestor.registry();
  for (Entity cursor = ParentOf(node); cursor != entt::null;
       cursor = registry.get<donner::components::TreeComponent>(cursor).parent()) {
    if (cursor == ancestor.entity()) {
      return true;
    }
  }

  return false;
}

void CheckCanInsert(EntityHandle parent, EntityHandle newNode) {
  CheckValid(parent, "parent is invalid");
  CheckValid(newNode, "newNode is invalid");
  CheckSameRegistry(parent, newNode);
  UTILS_RELEASE_ASSERT_MSG(parent.entity() != newNode.entity(),
                           "Cannot insert an entity as a child of itself");
  UTILS_RELEASE_ASSERT_MSG(!IsAncestorOf(newNode, parent),
                           "Cannot insert an ancestor as a child of its descendant");
}

components::RenderTreeState& GetRenderTreeState(EntityHandle handle) {
  auto& registry = *handle.registry();
  if (!registry.ctx().contains<components::RenderTreeState>()) {
    registry.ctx().emplace<components::RenderTreeState>();
  }
  return registry.ctx().get<components::RenderTreeState>();
}

void MarkNeedsFullRebuild(EntityHandle handle) {
  auto& renderState = GetRenderTreeState(handle);
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;
}

void MarkDirty(EntityHandle handle, uint16_t flags) {
  handle.get_or_emplace<components::DirtyFlagsComponent>().mark(flags);
}

void BumpMutationRevision(EntityHandle handle) {
  if (auto* context = handle.registry()->ctx().find<components::SVGDocumentContext>()) {
    context->bumpMutationRevision();
  }
}

std::optional<DocumentWriteAccess> AcquireWriteAccess(EntityHandle handle) {
  auto* context = handle.registry()->ctx().find<components::SVGDocumentContext>();
  if (context == nullptr) {
    return std::nullopt;
  }

  return context->writeAccess();
}

template <typename Func>
void ForSubtreeInclusive(EntityHandle root, const Func& func) {
  donner::components::ForAllChildrenRecursive(root, func);
}

void MarkDirtySubtree(EntityHandle root, uint16_t flags) {
  ForSubtreeInclusive(root, [flags](EntityHandle node) { MarkDirty(node, flags); });
}

DetachedNodeState* FindDetachedNodeState(Registry& registry) {
  auto* context = registry.ctx().find<components::SVGDocumentContext>();
  return context != nullptr ? &context->detachedNodeState() : nullptr;
}

void MarkAttachedSubtree(EntityHandle root) {
  DetachedNodeState* detachedNodeState = FindDetachedNodeState(*root.registry());
  ForSubtreeInclusive(root, [detachedNodeState](EntityHandle node) {
    auto& lifetime = node.get_or_emplace<components::NodeLifetimeComponent>();
    if (detachedNodeState != nullptr && !lifetime.isAttached()) {
      const Entity detachedRoot =
          lifetime.detachedRoot != entt::null ? lifetime.detachedRoot : node.entity();
      if (detachedRoot == node.entity()) {
        detachedNodeState->removeDetachedRoot(detachedRoot);
      }
    }
    lifetime.markAttached();
  });
}

void MarkDetachedSubtree(EntityHandle root, Entity detachedRoot) {
  DetachedNodeState* detachedNodeState = FindDetachedNodeState(*root.registry());
  ForSubtreeInclusive(root, [detachedNodeState, detachedRoot](EntityHandle node) {
    auto& lifetime = node.get_or_emplace<components::NodeLifetimeComponent>();
    if (detachedNodeState != nullptr && !lifetime.isAttached()) {
      const Entity oldDetachedRoot =
          lifetime.detachedRoot != entt::null ? lifetime.detachedRoot : node.entity();
      if (oldDetachedRoot != detachedRoot && oldDetachedRoot == node.entity()) {
        detachedNodeState->removeDetachedRoot(oldDetachedRoot);
      }
    }
    lifetime.markDetached(detachedRoot);
  });
}

void MarkDetachedSubtree(EntityHandle root) {
  MarkDetachedSubtree(root, root.entity());
  components::NodeLifetimeCollector::EnqueueDetachedRoot(*root.registry(), root.entity());
}

void AdoptParentLifetime(EntityHandle parent, EntityHandle child) {
  const auto* parentLifetime = parent.try_get<components::NodeLifetimeComponent>();
  if (parentLifetime == nullptr || parentLifetime->isAttached()) {
    MarkAttachedSubtree(child);
    return;
  }

  const Entity detachedRoot =
      parentLifetime->detachedRoot != entt::null ? parentLifetime->detachedRoot : parent.entity();
  MarkDetachedSubtree(child, detachedRoot);
}

void MarkOldParentDirty(EntityHandle node) {
  const Entity oldParent = ParentOf(node);
  if (oldParent == entt::null) {
    return;
  }

  EntityHandle oldParentHandle(*node.registry(), oldParent);
  MarkNeedsFullRebuild(oldParentHandle);
  MarkDirty(oldParentHandle, components::DirtyFlagsComponent::All);
}

void CheckReferenceChild(EntityHandle parent, EntityHandle referenceNode) {
  if (!referenceNode) {
    return;
  }

  CheckValid(referenceNode, "referenceNode is invalid");
  CheckSameRegistry(parent, referenceNode);
  UTILS_RELEASE_ASSERT_MSG(ParentOf(referenceNode) == parent.entity(),
                           "referenceNode must be a child of parent");
}

}  // namespace

void TreeMutation::InsertBefore(EntityHandle parent, EntityHandle newNode,
                                EntityHandle referenceNode) {
  CheckCanInsert(parent, newNode);
  CheckReferenceChild(parent, referenceNode);
  [[maybe_unused]] std::optional<DocumentWriteAccess> access = AcquireWriteAccess(parent);

  MarkOldParentDirty(newNode);

  parent.get<donner::components::TreeComponent>().insertBefore(
      *parent.registry(), newNode.entity(), referenceNode ? referenceNode.entity() : entt::null);

  MarkNeedsFullRebuild(parent);
  MarkDirty(parent, components::DirtyFlagsComponent::All);
  MarkDirtySubtree(newNode, components::DirtyFlagsComponent::All);
  AdoptParentLifetime(parent, newNode);
  BumpMutationRevision(parent);
}

void TreeMutation::AppendChild(EntityHandle parent, EntityHandle child) {
  CheckCanInsert(parent, child);
  [[maybe_unused]] std::optional<DocumentWriteAccess> access = AcquireWriteAccess(parent);

  MarkOldParentDirty(child);

  parent.get<donner::components::TreeComponent>().appendChild(*parent.registry(), child.entity());

  MarkNeedsFullRebuild(parent);
  MarkDirty(parent, components::DirtyFlagsComponent::All);
  MarkDirtySubtree(child, components::DirtyFlagsComponent::All);
  AdoptParentLifetime(parent, child);
  BumpMutationRevision(parent);
}

void TreeMutation::ReplaceChild(EntityHandle parent, EntityHandle newChild, EntityHandle oldChild) {
  CheckCanInsert(parent, newChild);
  CheckValid(oldChild, "oldChild is invalid");
  CheckSameRegistry(parent, oldChild);
  UTILS_RELEASE_ASSERT_MSG(ParentOf(oldChild) == parent.entity(),
                           "oldChild must be a child of parent");
  [[maybe_unused]] std::optional<DocumentWriteAccess> access = AcquireWriteAccess(parent);

  MarkOldParentDirty(newChild);

  parent.get<donner::components::TreeComponent>().replaceChild(
      *parent.registry(), newChild.entity(), oldChild.entity());

  MarkNeedsFullRebuild(parent);
  MarkDirty(parent, components::DirtyFlagsComponent::All);
  MarkDirtySubtree(newChild, components::DirtyFlagsComponent::All);
  AdoptParentLifetime(parent, newChild);
  if (newChild.entity() != oldChild.entity()) {
    MarkDetachedSubtree(oldChild);
    components::NodeLifetimeCollector::Collect(*parent.registry());
  }
  BumpMutationRevision(parent);
}

void TreeMutation::RemoveChild(EntityHandle parent, EntityHandle child) {
  CheckValid(parent, "parent is invalid");
  CheckValid(child, "child is invalid");
  CheckSameRegistry(parent, child);
  UTILS_RELEASE_ASSERT_MSG(ParentOf(child) == parent.entity(), "child must be a child of parent");
  [[maybe_unused]] std::optional<DocumentWriteAccess> access = AcquireWriteAccess(parent);

  parent.get<donner::components::TreeComponent>().removeChild(*parent.registry(), child.entity());

  MarkNeedsFullRebuild(parent);
  MarkDirty(parent, components::DirtyFlagsComponent::All);
  MarkDetachedSubtree(child);
  components::NodeLifetimeCollector::Collect(*parent.registry());
  BumpMutationRevision(parent);
}

void TreeMutation::Remove(EntityHandle entity) {
  CheckValid(entity, "entity is invalid");
  [[maybe_unused]] std::optional<DocumentWriteAccess> access = AcquireWriteAccess(entity);

  const Entity parentEntity = ParentOf(entity);
  if (parentEntity == entt::null) {
    return;
  }

  EntityHandle parent(*entity.registry(), parentEntity);
  entity.get<donner::components::TreeComponent>().remove(*entity.registry());

  MarkNeedsFullRebuild(parent);
  MarkDirty(parent, components::DirtyFlagsComponent::All);
  MarkDetachedSubtree(entity);
  components::NodeLifetimeCollector::Collect(*entity.registry());
  BumpMutationRevision(parent);
}

}  // namespace donner::svg::components
