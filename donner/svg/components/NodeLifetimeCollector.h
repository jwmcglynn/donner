#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/DocumentState.h"

namespace donner::svg::components {

/**
 * Collects detached SVG DOM subtrees when no public handles retain them.
 */
class NodeLifetimeCollector {
public:
  /**
   * Queue a detached subtree root for future collection.
   *
   * @param registry Document registry.
   * @param detachedRoot Root entity of the detached subtree.
   */
  static void EnqueueDetachedRoot(Registry& registry, Entity detachedRoot);

  /**
   * Return current detached-node diagnostics for a document registry.
   *
   * @param registry Document registry.
   */
  static DetachedNodeDiagnostics Diagnostics(Registry& registry);

  /**
   * Collect every queued detached subtree that has no public wrapper references.
   *
   * @param registry Document registry.
   */
  static void Collect(Registry& registry);
};

}  // namespace donner::svg::components
