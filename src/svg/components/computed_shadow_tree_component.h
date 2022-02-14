#pragma once

#include <map>
#include <unordered_map>

#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/shadow_entity_component.h"
#include "src/svg/components/shadow_tree_component.h"

// TODO: Automatically delete ComputedShadowTreeComponent when ShadowTreeComponent is removed.

namespace donner::svg {

struct ComputedShadowTreeComponent {
  ComputedShadowTreeComponent() {}

  Entity lightRoot() const { return lightRoot_; }

  /**
   * Destroy the instantiated shadow tree.
   *
   * @param registry The registry.
   */
  void teardown(Registry& registry) {
    // TODO: Ideally TreeComponents should automatically cleanup when the Entity is deleted.
    for (const auto& shadow : shadowEntities_) {
      registry.get<TreeComponent>(shadow).remove(registry);
    }

    registry.destroy(shadowEntities_.begin(), shadowEntities_.end());
    shadowEntities_.clear();
  }

  /**
   * Instantiate a shadow tree for the given target entity.
   *
   * @param registry The registry.
   * @param lightTarget Target entity to reflect in the shadow tree.
   * @param outWarnings If provided, warnings will be added to this vector.
   */
  void instantiate(Registry& registry, Entity lightTarget, std::vector<ParseError>* outWarnings) {
    teardown(registry);
    lightRoot_ = lightTarget;

    computeChildren(registry, entt::to_entity(registry, *this), lightTarget, outWarnings);
  }

private:
  void computeChildren(Registry& registry, Entity shadowParent, Entity lightTarget,
                       std::vector<ParseError>* outWarnings) {
    const Entity shadow = registry.create();
    const TreeComponent& lightTargetTree = registry.get<TreeComponent>(lightTarget);
    registry.emplace<TreeComponent>(shadow, lightTargetTree.type(), lightTargetTree.typeString());
    registry.emplace<ShadowEntityComponent>(shadow, lightTarget);
    registry.emplace<ComputedStyleComponent>(shadow);
    shadowEntities_.push_back(shadow);

    registry.get<TreeComponent>(shadowParent).appendChild(registry, shadow);

    // Iterate over all children and create Entities and ShadowEntityComponents for each of them for
    // the shadow tree.
    if (auto* nestedShadow = registry.try_get<ShadowTreeComponent>(lightTarget)) {
      if (auto targetEntity = nestedShadow->targetEntity(registry); targetEntity != entt::null) {
        computeChildren(registry, shadow, targetEntity, outWarnings);
      } else if (outWarnings) {
        ParseError err;
        err.reason = std::string("Failed to find target entity for nested shadow tree '") +
                     nestedShadow->href() + "'";
        outWarnings->emplace_back(std::move(err));
      }
    } else {
      for (auto child = lightTargetTree.firstChild(); child != entt::null;
           child = registry.get<TreeComponent>(child).nextSibling()) {
        computeChildren(registry, shadow, child, outWarnings);
      }
    }
  }

private:
  std::vector<Entity> shadowEntities_;

  /// The root of the "light" tree that this shadow tree reflects.
  Entity lightRoot_ = entt::null;
};

}  // namespace donner::svg
