#pragma once
/// @file

#include <map>
#include <span>
#include <unordered_map>

#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/offscreen_shadow_tree_component.h"
#include "src/svg/components/shadow_entity_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/style_component.h"  // TODO: DoNotInheritFillOrStrokeTag into its own file
#include "src/svg/components/tree_component.h"
#include "src/svg/core/shadow_branch.h"
#include "src/svg/graph/recursion_guard.h"
#include "src/svg/registry/registry.h"

// TODO: Automatically delete ComputedShadowTreeComponent when ShadowTreeComponent is removed.

namespace donner::svg {

namespace details {

/**
 * Get the target entity for a 'fill' or 'stroke' paint server reference.
 *
 * @param registry The registry to use.
 * @param lightTarget The element in the style tree to read properties from.
 * @param branchType The specific branch of the offscreen shadow tree to access.
 * @return std::tuple<Entity, RcString>
 */
// TODO: Move this to the .cpp file.
inline std::tuple<Entity, RcString> GetPaintTarget(Registry& registry, Entity lightTarget,
                                                   ShadowBranchType branchType) {
  if (const auto* offscreenShadow = registry.try_get<OffscreenShadowTreeComponent>(lightTarget)) {
    if (std::optional<ResolvedReference> target =
            offscreenShadow->branchTargetEntity(registry, branchType)) {
      return std::make_tuple(target->handle, offscreenShadow->branchHref(branchType).value());
    }
  }

  return std::make_tuple(entt::null, "");
}

}  // namespace details

struct ComputedShadowTreeComponent {
  using Branch = ShadowBranchType;

  ComputedShadowTreeComponent() {}

  Entity mainLightRoot() const { return mainBranch_ ? mainBranch_->lightTarget : entt::null; }

  size_t offscreenShadowCount() const { return branches_.size(); }

  std::span<const Entity> offscreenShadowEntities(size_t index) const {
    assert(index < offscreenShadowCount());
    return std::span<const Entity>(branches_[index].shadowEntities);
  }

  Entity offscreenShadowRoot(size_t index) const {
    assert(index < offscreenShadowCount());
    return branches_[index].shadowRoot();
  }

  std::optional<size_t> findOffscreenShadow(Branch branchType) const {
    for (size_t i = 0; i < offscreenShadowCount(); ++i) {
      if (branches_[i].branchType == branchType) {
        return i;
      }
    }

    return std::nullopt;
  }

  /**
   * Destroy the instantiated shadow tree.
   *
   * @param registry The registry.
   */
  void teardown(Registry& registry) {
    // TODO: Ideally TreeComponents should automatically cleanup when the Entity is deleted.
    if (mainBranch_) {
      for (const auto& shadow : mainBranch_->shadowEntities) {
        registry.get<TreeComponent>(shadow).remove(registry);
      }

      registry.destroy(mainBranch_->shadowEntities.begin(), mainBranch_->shadowEntities.end());
    }

    for (const auto& branch : branches_) {
      for (const auto& shadow : branch.shadowEntities) {
        registry.get<TreeComponent>(shadow).remove(registry);
      }

      registry.destroy(branch.shadowEntities.begin(), branch.shadowEntities.end());
    }

    mainBranch_ = std::nullopt;
    branches_.clear();
  }

  /**
   * Create a new computed shadow tree instance, such as the shadow tree for a <use> element or a
   * <pattern> element.
   *
   * For <pattern> paint servers, there may be multiple shadow trees originating from the same
   * entity, for both a 'fill' and a 'stroke', so this component can hold multiple shadow trees
   * simultaneously.
   *
   * @param registry The registry.
   * @param branchType Determines which branch of the tree to attach to. There may be multiple
   *   instances with a shadow tree, but only \ref Branch::Main will be traversed in the render
   *   tree.
   * @param lightTarget Target entity to reflect in the shadow tree.
   * @param href The value of the href attribute for the shadow tree, for diagnostics.
   * @param outWarnings If provided, warnings will be added to this vector.
   * @returns The index of the offscreen shadow tree, if \ref branchType is \ref
   *   Branch::HiddenOffscreen, or std::nullopt otherwise.
   */
  std::optional<size_t> populateInstance(Registry& registry, Branch branchType, Entity lightTarget,
                                         const RcString& href,
                                         std::vector<ParseError>* outWarnings) {
    assert((!mainBranch_ || branchType != Branch::Main) && "Only one main branch is allowed.");

    const Entity shadowHostEntity = entt::to_entity(registry, *this);

    BranchStorage storage;
    storage.branchType = branchType;
    storage.lightTarget = lightTarget;

    if (lightTarget == shadowHostEntity) {
      if (outWarnings) {
        ParseError err;
        err.reason = std::string("Shadow tree recursion detected, element references itself: '" +
                                 href + '"');
        outWarnings->emplace_back(std::move(err));
      }

      return std::nullopt;
    }

    std::set<Entity> shadowHostParents;
    for (auto cur = registry.get<TreeComponent>(shadowHostEntity).parent(); cur != entt::null;
         cur = registry.get<TreeComponent>(cur).parent()) {
      shadowHostParents.insert(cur);
    }

    if (shadowHostParents.count(lightTarget)) {
      if (outWarnings) {
        ParseError err;
        err.reason = std::string(
            "Shadow tree recursion detected, element directly references parent: '" + href + '"');
        outWarnings->emplace_back(std::move(err));
      }

      return std::nullopt;
    }

    RecursionGuard guard;
    computeChildren(registry, branchType, storage, guard, shadowHostEntity, lightTarget,
                    shadowHostParents, outWarnings);

    if (branchType == Branch::Main) {
      assert(!mainBranch_);
      mainBranch_ = std::move(storage);
      return std::nullopt;
    } else {
      const size_t result = branches_.size();
      branches_.emplace_back(std::move(storage));
      return result;
    }
  }

private:
  struct BranchStorage {
    /// Which branch this storage belongs to, there may be only one instance of each type.
    Branch branchType;
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

  Entity createShadowEntity(Registry& registry, ShadowBranchType branchType, BranchStorage& storage,
                            Entity lightTarget, Entity shadowParent) {
    const Entity shadow = registry.create();
    const TreeComponent& lightTargetTree = registry.get<TreeComponent>(lightTarget);
    registry.emplace<TreeComponent>(shadow, lightTargetTree.type(), lightTargetTree.typeString());
    registry.emplace<ShadowEntityComponent>(shadow, lightTarget);
    registry.emplace<ComputedStyleComponent>(shadow);

    // This property is special, and is copied into the shadow tree to be used for style
    // inheritance.
    if (registry.all_of<DoNotInheritFillOrStrokeTag>(lightTarget)) {
      registry.emplace<DoNotInheritFillOrStrokeTag>(shadow);
    }

    // Don't attach to the parent if this is the start of an offscreen tree.
    if (branchType == Branch::Main || lightTarget != storage.lightTarget) {
      registry.get<TreeComponent>(shadowParent).appendChild(registry, shadow);
    }

    storage.shadowEntities.push_back(shadow);
    return shadow;
  }

  void computeChildren(Registry& registry, ShadowBranchType branchType, BranchStorage& storage,
                       RecursionGuard& guard, Entity shadowParent, Entity lightTarget,
                       const std::set<Entity>& shadowHostParents,
                       std::vector<ParseError>* outWarnings) {
    auto validateNoRecursion = [&guard, &shadowHostParents, outWarnings](
                                   const RcString& href, Entity targetEntity) -> bool {
      if (shadowHostParents.count(targetEntity)) {
        if (outWarnings) {
          ParseError err;
          err.reason = std::string(
              "Shadow tree indirect recursion detected, element "
              "references a shadow host parent: '" +
              href + "'");
          outWarnings->emplace_back(std::move(err));
        }

        return false;
      } else if (guard.hasRecursion(targetEntity)) {
        if (outWarnings) {
          ParseError err;
          err.reason = std::string("Shadow tree recursion detected, ignoring shadow tree for '" +
                                   href + '"');
          outWarnings->emplace_back(std::move(err));
        }

        return false;
      }

      return true;
    };

    // Validate we don't have recursion from 'fill' or 'stroke' paint servers.
    if (branchType != Branch::Main) {
      if (auto [targetEntity, href] = details::GetPaintTarget(registry, lightTarget, branchType);
          targetEntity != entt::null) {
        if (!validateNoRecursion(href, targetEntity)) {
          return;
        }
      }
    }

    // Iterate over all children and create Entities and ShadowEntityComponents for each of them for
    // the main shadow tree.
    if (const auto* nestedShadow = registry.try_get<ShadowTreeComponent>(lightTarget)) {
      if (auto targetEntity = nestedShadow->mainTargetEntity(registry)) {
        if (!validateNoRecursion(nestedShadow->mainHref().value_or(""), targetEntity.value())) {
          return;
        }

        const Entity shadow =
            createShadowEntity(registry, branchType, storage, lightTarget, shadowParent);

        RecursionGuard childGuard = guard.with(targetEntity.value());
        computeChildren(registry, branchType, storage, childGuard, shadow, targetEntity->handle,
                        shadowHostParents, outWarnings);
      } else if (outWarnings) {
        ParseError err;
        err.reason = std::string("Failed to find target entity for nested shadow tree '") +
                     nestedShadow->mainHref().value_or("") + "'";
        outWarnings->emplace_back(std::move(err));
      }
    } else {
      const Entity shadow =
          createShadowEntity(registry, branchType, storage, lightTarget, shadowParent);

      for (auto child = registry.get<TreeComponent>(lightTarget).firstChild(); child != entt::null;
           child = registry.get<TreeComponent>(child).nextSibling()) {
        computeChildren(registry, branchType, storage, guard, shadow, child, shadowHostParents,
                        outWarnings);
      }
    }
  }

private:
  /// If set, points to main branch of the shadow tree, or \c std::nullopt if there is no main
  /// branch instantiated.
  std::optional<BranchStorage> mainBranch_;

  /// Storage for the shadow trees, every element except \ref mainBranch_ is in the \ref
  /// Branch::HiddenOffscreen branch.
  std::vector<BranchStorage> branches_;
};

}  // namespace donner::svg
