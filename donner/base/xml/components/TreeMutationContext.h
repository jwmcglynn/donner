#pragma once
/// @file

#include <functional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/components/TreeComponent.h"

namespace donner::components {

/**
 * Document-local hooks for tree mutations.
 *
 * Always installed in `Registry::ctx()` by the owning document model:
 * \ref donner::xml::XMLDocument installs the `Default*` callbacks below (which operate on
 * \ref TreeComponent directly), and higher-level models such as SVGDocument overwrite the
 * individual callbacks after construction to layer invalidation and lifetime tracking on top.
 * \ref donner::xml::XMLNode mutation methods always go through the context, so the lookup never
 * needs to fall back to a direct \ref TreeComponent path -
 * `Registry::ctx().contains<TreeMutationContext>()` is an invariant of any registry exposed
 * through one of the document facades.
 */
struct TreeMutationContext {
  /// Default ctor installs the basic XML callbacks. Higher-level models (SVGDocument) overwrite
  /// the individual function fields after construction.
  TreeMutationContext()
      : insertBefore(DefaultInsertBefore),
        appendChild(DefaultAppendChild),
        replaceChild(DefaultReplaceChild),
        removeChild(DefaultRemoveChild),
        remove(DefaultRemove) {}

  /// Callback for `insertBefore(parent, newNode, referenceNode)`.
  std::function<void(EntityHandle parent, EntityHandle newNode, EntityHandle referenceNode)>
      insertBefore;

  /// Callback for `appendChild(parent, child)`.
  std::function<void(EntityHandle parent, EntityHandle child)> appendChild;

  /// Callback for `replaceChild(parent, newChild, oldChild)`.
  std::function<void(EntityHandle parent, EntityHandle newChild, EntityHandle oldChild)>
      replaceChild;

  /// Callback for `removeChild(parent, child)`.
  std::function<void(EntityHandle parent, EntityHandle child)> removeChild;

  /// Callback for `remove(entity)`.
  std::function<void(EntityHandle entity)> remove;

  /// Basic XML defaults - operate on \ref TreeComponent directly.
  static void DefaultInsertBefore(EntityHandle parent, EntityHandle newNode,
                                  EntityHandle referenceNode) {
    parent.get<TreeComponent>().insertBefore(*parent.registry(), newNode.entity(),
                                             referenceNode ? referenceNode.entity() : entt::null);
  }

  /// Append a child through the default ECS tree operation.
  /// @param parent Destination parent in the same registry as the child.
  /// @param child Element to append.
  static void DefaultAppendChild(EntityHandle parent, EntityHandle child) {
    parent.get<TreeComponent>().appendChild(*parent.registry(), child.entity());
  }

  /// Replace a child through the default ECS tree operation.
  /// @param parent Parent containing the old child.
  /// @param newChild Replacement child in the same registry.
  /// @param oldChild Child to replace.
  static void DefaultReplaceChild(EntityHandle parent, EntityHandle newChild,
                                  EntityHandle oldChild) {
    parent.get<TreeComponent>().replaceChild(*parent.registry(), newChild.entity(),
                                             oldChild.entity());
  }

  /// Detach a child through the default ECS tree operation.
  /// @param parent Parent containing the child.
  /// @param child Child to detach.
  static void DefaultRemoveChild(EntityHandle parent, EntityHandle child) {
    parent.get<TreeComponent>().removeChild(*parent.registry(), child.entity());
  }

  /// Detach an entity through the default ECS tree operation.
  /// @param entity Entity to remove from its parent.
  static void DefaultRemove(EntityHandle entity) {
    entity.get<TreeComponent>().remove(*entity.registry());
  }
};

}  // namespace donner::components
