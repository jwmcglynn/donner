#pragma once
/// @file

#include <span>

#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/registry/Registry.h"

// TODO: Automatically delete ComputedShadowTreeComponent when ShadowTreeComponent is removed.

namespace donner::svg::components {

struct ComputedShadowTreeComponent {
  ComputedShadowTreeComponent() {}

  Entity mainLightRoot() const { return mainBranch ? mainBranch->lightTarget : entt::null; }

  size_t offscreenShadowCount() const { return branches.size(); }

  std::span<const Entity> offscreenShadowEntities(size_t index) const {
    assert(index < offscreenShadowCount());
    return std::span<const Entity>(branches[index].shadowEntities);
  }

  Entity offscreenShadowRoot(size_t index) const {
    assert(index < offscreenShadowCount());
    return branches[index].shadowRoot();
  }

  std::optional<size_t> findOffscreenShadow(ShadowBranchType branchType) const {
    for (size_t i = 0; i < offscreenShadowCount(); ++i) {
      if (branches[i].branchType == branchType) {
        return i;
      }
    }

    return std::nullopt;
  }

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

  /// Storage for the shadow trees, every element except \ref mainBranch is in the \ref
  /// ShadowBranchType::HiddenOffscreen branch.
  std::vector<BranchStorage> branches;
};

}  // namespace donner::svg::components
