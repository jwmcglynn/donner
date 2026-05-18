#pragma once
/// @file

#include <functional>

#include "donner/base/EcsRegistry.h"

namespace donner::components {

/**
 * Optional document-local hooks for tree mutations.
 *
 * \ref XMLNode uses these callbacks when a document installs them in `Registry::ctx()`. Plain XML
 * documents leave the hooks absent and mutate \ref TreeComponent directly. Higher-level document
 * models, such as SVG, use the hooks to layer invalidation and lifetime tracking over the shared
 * XML tree API without making the base XML library depend on SVG.
 */
struct TreeMutationContext {
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
};

}  // namespace donner::components
