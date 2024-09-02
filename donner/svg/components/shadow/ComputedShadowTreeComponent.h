#pragma once
/// @file

#include <span>

#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/registry/Registry.h"

// TODO(jwmcglynn): Automatically delete ComputedShadowTreeComponent when ShadowTreeComponent is
// removed.

namespace donner::svg::components {

/**
 * An instantiated \ref ShadowTreeComponent, which points to the roots of parallel entity trees.
 *
 * This component attaches to the shadow host (where the tree is instantiated), and contains one or
 * more shadow trees. Each shadow tree is a tree of entities, which are all children of the shadow
 * host.
 *
 * Each entity in the shadow tree has a \ref ShadowEntityComponent attached.
 */
struct ComputedShadowTreeComponent {
  /// Default constructor.
  ComputedShadowTreeComponent() = default;

  /// Get the target element for the main branch root, or \c entt::null if there is no main branch.
  Entity mainLightRoot() const { return mainBranch ? mainBranch->lightTarget : entt::null; }

  /// Get the number of additional shadow trees (offscreen trees).
  size_t offscreenShadowCount() const { return branches.size(); }

  /**
   * Get the target element for the offscreen shadow tree.
   *
   * @param index The index of the offscreen shadow tree, must be less than \ref
   * offscreenShadowCount().
   */
  std::span<const Entity> offscreenShadowEntities(size_t index) const {
    assert(index < offscreenShadowCount());
    return std::span<const Entity>(branches[index].shadowEntities);
  }

  /**
   * Get the target element for the offscreen shadow tree.
   *
   * @param index The index of the offscreen shadow tree, must be less than \ref
   * offscreenShadowCount().
   */
  Entity offscreenShadowRoot(size_t index) const {
    assert(index < offscreenShadowCount());
    return branches[index].shadowRoot();
  }

  /**
   * Find the index of the offscreen shadow tree with the given branch type.
   *
   * @param branchType The branch type to search for.
   * @returns The index of the offscreen shadow tree, or \c std::nullopt if not found.
   */
  std::optional<size_t> findOffscreenShadow(ShadowBranchType branchType) const {
    for (size_t i = 0; i < offscreenShadowCount(); ++i) {
      if (branches[i].branchType == branchType) {
        return i;
      }
    }

    return std::nullopt;
  }

  /// Storage for a single shadow tree.
  struct BranchStorage {
    /// Which branch this storage belongs to, there may be only one instance of each type.
    ShadowBranchType branchType;
    /// The root of the "light" tree that this shadow tree reflects.
    Entity lightTarget;
    /// All of the entities in this shadow tree, in order of traversal.
    std::vector<Entity> shadowEntities;

    /// The root of the shadow tree.
    Entity shadowRoot() const {
      if (!shadowEntities.empty()) {
        return shadowEntities.front();
      } else {
        return entt::null;
      }
    }
  };

  /// If set, points to main branch of the shadow tree, or \c std::nullopt if there is no main
  /// branch instantiated.
  std::optional<BranchStorage> mainBranch;

  /// Storage for additional shadow trees, such as \ref ShadowBranchType::OffscreenFill or \ref
  /// OffscreenStroke.
  std::vector<BranchStorage> branches;
};

}  // namespace donner::svg::components
