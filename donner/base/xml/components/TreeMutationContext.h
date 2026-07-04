#pragma once
/// @file

#include <functional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/components/TreeComponent.h"

namespace donner::components {

/**
 * Document-local hooks for tree mutations.
 *
 * Always installed in `Registry::ctx()` by the owning document model: \ref XMLDocument installs
 * the `Default*` callbacks below (which operate on \ref TreeComponent directly), and higher-level
 * models such as SVGDocument overwrite the individual callbacks after construction to layer
 * invalidation and lifetime tracking on top. \ref XMLNode mutation methods always go through the
 * context, so the lookup never needs to fall back to a direct \ref TreeComponent path -
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

  static void DefaultAppendChild(EntityHandle parent, EntityHandle child) {
    parent.get<TreeComponent>().appendChild(*parent.registry(), child.entity());
  }

  static void DefaultReplaceChild(EntityHandle parent, EntityHandle newChild,
                                  EntityHandle oldChild) {
    parent.get<TreeComponent>().replaceChild(*parent.registry(), newChild.entity(),
                                             oldChild.entity());
  }

  static void DefaultRemoveChild(EntityHandle parent, EntityHandle child) {
    parent.get<TreeComponent>().removeChild(*parent.registry(), child.entity());
  }

  static void DefaultRemove(EntityHandle entity) {
    entity.get<TreeComponent>().remove(*entity.registry());
  }
};

}  // namespace donner::components
