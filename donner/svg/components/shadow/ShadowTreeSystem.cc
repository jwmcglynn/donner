#include "donner/svg/components/shadow/ShadowTreeSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/shadow/OffscreenShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"

namespace donner::svg::components {

namespace {

/**
 * Get the target entity for a 'fill' or 'stroke' paint server reference.
 *
 * @param registry The registry to use.
 * @param lightTarget The element in the style tree to read properties from.
 * @param branchType The specific branch of the offscreen shadow tree to access.
 * @return std::tuple<Entity, RcString>
 */
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

}  // namespace

void ShadowTreeSystem::teardown(Registry& registry, ComputedShadowTreeComponent& shadow) {
  // TODO(jwmcglynn): Ideally TreeComponents should automatically cleanup when the Entity is
  // deleted.
  if (shadow.mainBranch) {
    for (const auto& shadow : shadow.mainBranch->shadowEntities) {
      registry.get<donner::components::TreeComponent>(shadow).remove(registry);
    }

    registry.destroy(shadow.mainBranch->shadowEntities.begin(),
                     shadow.mainBranch->shadowEntities.end());
  }

  for (const auto& branch : shadow.branches) {
    for (const auto& shadow : branch.shadowEntities) {
      registry.get<donner::components::TreeComponent>(shadow).remove(registry);
    }

    registry.destroy(branch.shadowEntities.begin(), branch.shadowEntities.end());
  }

  shadow.mainBranch = std::nullopt;
  shadow.branches.clear();
}

std::optional<size_t> ShadowTreeSystem::populateInstance(
    EntityHandle entity, ComputedShadowTreeComponent& shadow, ShadowBranchType branchType,
    Entity lightTarget, const RcString& href, std::vector<parser::ParseError>* outWarnings) {
  assert((!shadow.mainBranch || branchType != ShadowBranchType::Main) &&
         "Only one main branch is allowed.");

  ComputedShadowTreeComponent::BranchStorage storage;
  storage.branchType = branchType;
  storage.lightTarget = lightTarget;

  if (lightTarget == entity) {
    if (outWarnings) {
      parser::ParseError err;
      err.reason =
          std::string("Shadow tree recursion detected, element references itself: '" + href + '"');
      outWarnings->emplace_back(std::move(err));
    }

    return std::nullopt;
  }

  std::set<Entity> shadowHostParents;
  for (auto cur = entity.get<donner::components::TreeComponent>().parent(); cur != entt::null;
       cur = entity.registry()->get<donner::components::TreeComponent>(cur).parent()) {
    shadowHostParents.insert(cur);
  }

  if (shadowHostParents.count(lightTarget)) {
    if (outWarnings) {
      parser::ParseError err;
      err.reason = std::string(
          "Shadow tree recursion detected, element directly references parent: '" + href + '"');
      outWarnings->emplace_back(std::move(err));
    }

    return std::nullopt;
  }

  RecursionGuard guard;
  computeChildren(*entity.registry(), branchType, storage, guard, entity, lightTarget,
                  shadowHostParents, outWarnings);

  if (branchType == ShadowBranchType::Main) {
    assert(!shadow.mainBranch);
    shadow.mainBranch = std::move(storage);
    return std::nullopt;
  } else {
    const size_t result = shadow.branches.size();
    shadow.branches.emplace_back(std::move(storage));
    return result;
  }
}

Entity ShadowTreeSystem::createShadowEntity(Registry& registry, ShadowBranchType branchType,
                                            ComputedShadowTreeComponent::BranchStorage& storage,
                                            Entity lightTarget, Entity shadowParent) {
  const Entity shadow = registry.create();
  const auto& lightTargetTree = registry.get<donner::components::TreeComponent>(lightTarget);
  registry.emplace<donner::components::TreeComponent>(shadow, lightTargetTree.tagName());
  registry.emplace<ShadowEntityComponent>(shadow, lightTarget);
  registry.emplace<ComputedStyleComponent>(shadow);

  // This property is special, and is copied into the shadow tree to be used for style
  // inheritance.
  if (registry.all_of<DoNotInheritFillOrStrokeTag>(lightTarget)) {
    registry.emplace<DoNotInheritFillOrStrokeTag>(shadow);
  }

  // Don't attach to the parent if this is the start of an offscreen tree.
  if (branchType == ShadowBranchType::Main || lightTarget != storage.lightTarget) {
    registry.get<donner::components::TreeComponent>(shadowParent).appendChild(registry, shadow);
  }

  storage.shadowEntities.push_back(shadow);
  return shadow;
}

void ShadowTreeSystem::computeChildren(Registry& registry, ShadowBranchType branchType,
                                       ComputedShadowTreeComponent::BranchStorage& storage,
                                       RecursionGuard& guard, Entity shadowParent,
                                       Entity lightTarget,
                                       const std::set<Entity>& shadowHostParents,
                                       std::vector<parser::ParseError>* outWarnings) {
  auto validateNoRecursion = [&guard, &shadowHostParents, outWarnings](
                                 const RcString& href, Entity targetEntity) -> bool {
    if (shadowHostParents.count(targetEntity)) {
      if (outWarnings) {
        parser::ParseError err;
        err.reason = std::string(
            "Shadow tree indirect recursion detected, element "
            "references a shadow host parent: '" +
            href + "'");
        outWarnings->emplace_back(std::move(err));
      }

      return false;
    } else if (guard.hasRecursion(targetEntity)) {
      if (outWarnings) {
        parser::ParseError err;
        err.reason =
            std::string("Shadow tree recursion detected, ignoring shadow tree for '" + href + '"');
        outWarnings->emplace_back(std::move(err));
      }

      return false;
    }

    return true;
  };

  // Validate we don't have recursion from 'fill' or 'stroke' paint servers.
  if (branchType != ShadowBranchType::Main) {
    if (auto [targetEntity, href] = GetPaintTarget(registry, lightTarget, branchType);
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
      parser::ParseError err;
      err.reason = std::string("Failed to find target entity for nested shadow tree '") +
                   nestedShadow->mainHref().value_or("") + "'";
      outWarnings->emplace_back(std::move(err));
    }
  } else {
    const Entity shadow =
        createShadowEntity(registry, branchType, storage, lightTarget, shadowParent);

    for (auto child = registry.get<donner::components::TreeComponent>(lightTarget).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      RecursionGuard childGuard = guard.with(child);
      computeChildren(registry, branchType, storage, guard, shadow, child, shadowHostParents,
                      outWarnings);
    }
  }
}

}  // namespace donner::svg::components
