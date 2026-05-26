#pragma once
/// @file

#include <atomic>
#include <cstdint>
#include <memory>

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/// Shared control block retained by public SVG DOM element wrappers.
struct NodeExternalRefState {
  std::atomic<bool> isDetached = true;  //!< Fast release-path hint for collection.
};

/**
 * Tracks whether an SVG DOM node is attached to the document tree or currently detached.
 *
 * This is the first step toward collecting removed subtrees without breaking public DOM handles.
 * Attached nodes are reachable from the document root. Detached nodes retain their internal
 * subtree links, but their detached root is recorded so a later collector can reclaim the whole
 * subtree once no public handles or render snapshots retain it.
 *
 * @ingroup ecs_components
 */
struct NodeLifetimeComponent {
  /// Tree reachability state for the node.
  enum class TreeState : uint8_t {
    Attached,    //!< Reachable from the document root.
    Detached,    //!< Not reachable from the document root.
    Destroying,  //!< Being reclaimed by the detached-subtree collector.
  };

  /// Create lifetime state with a fresh public-handle control block.
  NodeLifetimeComponent() = default;

  /**
   * Copy lifetime metadata while starting a fresh public-handle control block.
   *
   * Component copies are used for cloned ECS storage; public wrappers for the source entity must
   * not retain the copied entity.
   *
   * @param other Source lifetime component.
   */
  NodeLifetimeComponent(const NodeLifetimeComponent& other)
      : treeState(other.treeState),
        detachedRoot(other.detachedRoot),
        generation(other.generation) {}

  /**
   * Assign lifetime metadata while starting a fresh public-handle control block.
   *
   * @param other Source lifetime component.
   */
  NodeLifetimeComponent& operator=(const NodeLifetimeComponent& other) {
    if (this != &other) {
      treeState = other.treeState;
      detachedRoot = other.detachedRoot;
      generation = other.generation;
      externalRefs = std::make_shared<NodeExternalRefState>();
    }
    return *this;
  }

  /// Moving lifetime components is allowed.
  NodeLifetimeComponent(NodeLifetimeComponent&& other) noexcept = default;

  /// Moving lifetime components is allowed.
  NodeLifetimeComponent& operator=(NodeLifetimeComponent&& other) noexcept = default;

  TreeState treeState = TreeState::Detached;  //!< Current tree reachability.
  Entity detachedRoot = entt::null;           //!< Detached subtree root, or null if attached.
  std::uint32_t generation = 0;               //!< Reserved for future lifetime-aware handles.
  std::shared_ptr<NodeExternalRefState> externalRefs =
      std::make_shared<NodeExternalRefState>();  //!< Public DOM wrapper control block.

  /// Mark this node as attached to the document tree.
  void markAttached() {
    treeState = TreeState::Attached;
    detachedRoot = entt::null;
    externalRefs->isDetached.store(false, std::memory_order_release);
  }

  /**
   * Mark this node as part of a detached subtree.
   *
   * @param root Root entity of the detached subtree.
   */
  void markDetached(Entity root) {
    treeState = TreeState::Detached;
    detachedRoot = root;
    externalRefs->isDetached.store(true, std::memory_order_release);
  }

  /// Mark this node as being reclaimed by the detached-subtree collector.
  void markDestroying() {
    treeState = TreeState::Destroying;
    externalRefs->isDetached.store(true, std::memory_order_release);
  }

  /// Returns true if this node is currently attached to the document tree.
  bool isAttached() const { return treeState == TreeState::Attached; }

  /// Number of public DOM wrappers currently retaining this node.
  std::uint32_t externalRefCount() const {
    const long refs = externalRefs.use_count();
    return refs > 1 ? static_cast<std::uint32_t>(refs - 1) : 0;
  }
};

/**
 * Returns true when an entity is currently part of the live document tree.
 *
 * Entities without a \ref NodeLifetimeComponent are treated as attached so legacy raw-ECS tests
 * and transitional call sites keep their previous behavior until every SVG entity is lifetime
 * annotated.
 *
 * @param registry Registry that owns \p entity.
 * @param entity Entity to inspect.
 */
inline bool IsEntityAttachedToDocument(const Registry& registry, Entity entity) {
  if (entity == entt::null || !registry.valid(entity)) {
    return false;
  }

  const auto* lifetime = registry.try_get<NodeLifetimeComponent>(entity);
  return lifetime == nullptr || lifetime->isAttached();
}

}  // namespace donner::svg::components
