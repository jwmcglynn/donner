#include "donner/svg/components/shadow/ShadowTreeSystem.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/shadow/OffscreenShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"

namespace donner::svg::components {

namespace {

struct ShadowExpansionCost {
  std::size_t generatedEntities = 0;
  std::size_t maximumReferenceDepth = 0;
  std::size_t maximumTraversalDepth = 0;
};

std::optional<ShadowExpansionCost> PreflightShadowExpansion(
    Registry& registry, Entity lightTarget, const std::set<Entity>& shadowHostParents) {
  struct WorkItem {
    Entity entity = entt::null;
    Entity leavingReference = entt::null;
    std::size_t referenceDepth = 0;
    std::size_t traversalDepth = 0;
  };

  std::vector<WorkItem> stack = {{.entity = lightTarget, .referenceDepth = 0, .traversalDepth = 1}};
  std::unordered_set<Entity> activeReferences;
  ShadowExpansionCost cost;

  while (!stack.empty()) {
    const WorkItem work = stack.back();
    stack.pop_back();
    if (work.leavingReference != entt::null) {
      activeReferences.erase(work.leavingReference);
      continue;
    }
    if (work.entity == entt::null) {
      continue;
    }
    if (work.traversalDepth > ShadowTreeResourceBudget::kMaximumTraversalDepth) {
      return std::nullopt;
    }

    const auto* nestedShadow = registry.try_get<ShadowTreeComponent>(work.entity);
    std::optional<ResolvedReference> nestedTarget;
    if (nestedShadow != nullptr) {
      nestedTarget = nestedShadow->mainTargetEntity(registry);
      if (!nestedTarget.has_value() || shadowHostParents.count(nestedTarget->handle) != 0 ||
          activeReferences.count(nestedTarget->handle) != 0) {
        continue;
      }
    }

    if (cost.generatedEntities >= ShadowTreeResourceBudget::kMaximumGeneratedEntities) {
      return std::nullopt;
    }
    ++cost.generatedEntities;
    cost.maximumReferenceDepth = std::max(cost.maximumReferenceDepth, work.referenceDepth);
    cost.maximumTraversalDepth = std::max(cost.maximumTraversalDepth, work.traversalDepth);

    if (nestedTarget.has_value()) {
      const std::size_t nextReferenceDepth = work.referenceDepth + 1;
      if (nextReferenceDepth > ShadowTreeResourceBudget::kMaximumReferenceDepth) {
        return std::nullopt;
      }
      activeReferences.insert(nestedTarget->handle);
      stack.push_back({.leavingReference = nestedTarget->handle});
      stack.push_back({.entity = nestedTarget->handle,
                       .referenceDepth = nextReferenceDepth,
                       .traversalDepth = work.traversalDepth + 1});
      continue;
    }

    const auto& tree = registry.get<donner::components::TreeComponent>(work.entity);
    for (Entity child = tree.lastChild(); child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).previousSibling()) {
      stack.push_back({.entity = child,
                       .referenceDepth = work.referenceDepth,
                       .traversalDepth = work.traversalDepth + 1});
    }
  }

  return cost;
}

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

std::set<Entity> CollectShadowHostParents(EntityHandle entity) {
  std::set<Entity> parents;
  for (Entity current = entity.get<donner::components::TreeComponent>().parent();
       current != entt::null;
       current = entity.registry()->get<donner::components::TreeComponent>(current).parent()) {
    parents.insert(current);
  }
  return parents;
}

bool AdmitShadowExpansion(Registry& registry, Entity lightTarget,
                          const std::set<Entity>& shadowHostParents,
                          ShadowTreeResourceBudget& resourceBudget) {
  const auto cost = PreflightShadowExpansion(registry, lightTarget, shadowHostParents);
  if (!cost) {
    resourceBudget.reject();
    return false;
  }
  if (!resourceBudget.reserve(cost->generatedEntities, cost->maximumReferenceDepth,
                              cost->maximumTraversalDepth)) {
    resourceBudget.reject();
    return false;
  }
  return true;
}

}  // namespace

bool ShadowTreeResourceBudget::reserve(std::size_t generatedEntities, std::size_t referenceDepth,
                                       std::size_t traversalDepth) {
  if (rejected_ || instances_ >= kMaximumInstances ||
      generatedEntities > kMaximumGeneratedEntities - generatedEntities_ ||
      referenceDepth > kMaximumReferenceDepth || traversalDepth > kMaximumTraversalDepth) {
    rejected_ = true;
    return false;
  }

  ++instances_;
  generatedEntities_ += generatedEntities;
  return true;
}

void ShadowTreeSystem::beginRebuild(Registry& registry) {
  if (registry.ctx().contains<ShadowTreeResourceBudget>()) {
    registry.ctx().get<ShadowTreeResourceBudget>().reset();
  } else {
    registry.ctx().emplace<ShadowTreeResourceBudget>();
  }
}

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

void ShadowTreeSystem::teardownInstances(EntityHandle handle) {
  // TODO(jwmcglynn): This only tears down instances recorded on `handle` itself. A shadow tree
  // whose clones are themselves shadow hosts records those nested instances on the clone
  // entities, and destroying the clones here drops those records without destroying what they
  // point at. The leak is bounded and unreachable in practice today because the render path
  // clears every ComputedShadowTreeComponent through the teardown loop in
  // `RenderingContext::ensureComputedComponents()` before rebuilding, but a caller that relies on
  // this function alone would leak. Recursing here needs the nested entities to be reachable
  // without a full registry scan.
  if (auto* shadow = handle.try_get<ComputedShadowTreeComponent>()) {
    teardown(*handle.registry(), *shadow);
  }
}

std::optional<size_t> ShadowTreeSystem::populateInstance(EntityHandle entity,
                                                         ComputedShadowTreeComponent& shadow,
                                                         ShadowBranchType branchType,
                                                         Entity lightTarget, const RcString& href,
                                                         ParseWarningSink& warningSink) {
  assert((!shadow.mainBranch || branchType != ShadowBranchType::Main) &&
         "Only one main branch is allowed.");

  ComputedShadowTreeComponent::BranchStorage storage;
  storage.branchType = branchType;
  storage.lightTarget = lightTarget;

  if (lightTarget == entity) {
    ParseDiagnostic err;
    err.reason =
        std::string("Shadow tree recursion detected, element references itself: '" + href + '"');
    warningSink.add(std::move(err));

    return std::nullopt;
  }

  const std::set<Entity> shadowHostParents = CollectShadowHostParents(entity);

  if (shadowHostParents.count(lightTarget)) {
    ParseDiagnostic err;
    err.reason = std::string(
        "Shadow tree recursion detected, element directly references parent: '" + href + '"');
    warningSink.add(std::move(err));

    return std::nullopt;
  }

  Registry& registry = *entity.registry();

  auto& resourceBudget = registry.ctx().contains<ShadowTreeResourceBudget>()
                             ? registry.ctx().get<ShadowTreeResourceBudget>()
                             : registry.ctx().emplace<ShadowTreeResourceBudget>();
  if (!AdmitShadowExpansion(registry, lightTarget, shadowHostParents, resourceBudget)) {
    ParseDiagnostic err;
    err.reason = "Shadow tree resource budget exceeded for '" + std::string(href) + "'";
    warningSink.add(std::move(err));
    return std::nullopt;
  }

  RecursionGuard guard;
  Entity shadowEntity = createShadowAndChildren(registry, branchType, storage, guard, entity,
                                                lightTarget, shadowHostParents, warningSink);

  if (shadowEntity != entt::null) {
    registry.emplace<ShadowTreeRootComponent>(shadowEntity, entity);
  }

  // Handle sized element inheritance for &lt;use&gt; -> &lt;symbol&gt; shadow trees.
  if (branchType == ShadowBranchType::Main && sizedElementHandler_) {
    // Use the provided sized element handler callback to process sized elements
    // This avoids a direct dependency on the LayoutSystem
    ParseWarningSink disabledSink = ParseWarningSink::Disabled();
    sizedElementHandler_(registry, shadowEntity, entity, lightTarget, branchType, disabledSink);
  }

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

Entity ShadowTreeSystem::createShadowAndChildren(
    Registry& registry, ShadowBranchType branchType,
    ComputedShadowTreeComponent::BranchStorage& storage, RecursionGuard& guard, Entity shadowParent,
    Entity lightTarget, const std::set<Entity>& shadowHostParents, ParseWarningSink& warningSink) {
  auto validateNoRecursion = [&guard, &shadowHostParents, &warningSink](
                                 const RcString& href, Entity targetEntity) -> bool {
    if (shadowHostParents.count(targetEntity)) {
      ParseDiagnostic err;
      err.reason = std::string(
          "Shadow tree indirect recursion detected, element "
          "references a shadow host parent: '" +
          href + "'");
      warningSink.add(std::move(err));

      return false;
    } else if (guard.hasRecursion(targetEntity)) {
      ParseDiagnostic err;
      err.reason =
          std::string("Shadow tree recursion detected, ignoring shadow tree for '" + href + '"');
      warningSink.add(std::move(err));

      return false;
    }

    return true;
  };

  // Validate we don't have recursion from 'fill' or 'stroke' paint servers.
  if (branchType != ShadowBranchType::Main) {
    if (auto [targetEntity, href] = GetPaintTarget(registry, lightTarget, branchType);
        targetEntity != entt::null) {
      if (!validateNoRecursion(href, targetEntity)) {
        return entt::null;
      }
    }
  }

  // Iterate over all children and create Entities and ShadowEntityComponents for each of them for
  // the main shadow tree.
  if (const auto* nestedShadow = registry.try_get<ShadowTreeComponent>(lightTarget)) {
    if (auto targetEntity = nestedShadow->mainTargetEntity(registry)) {
      if (!validateNoRecursion(nestedShadow->mainHref().value_or(""), targetEntity.value())) {
        return entt::null;
      }

      // TODO: Factor out common functionality to create a new tree w/ populateInstance
      const Entity shadow =
          createShadowEntity(registry, branchType, storage, lightTarget, shadowParent);

      RecursionGuard childGuard = guard.with(targetEntity.value());
      const Entity nestedShadowEntity =
          createShadowAndChildren(registry, branchType, storage, childGuard, shadow,
                                  targetEntity->handle, shadowHostParents, warningSink);

      // Handle sized element inheritance for <use> -> <symbol> shadow trees
      if (branchType == ShadowBranchType::Main && sizedElementHandler_) {
        // Use the provided sized element handler callback to process sized elements
        // This avoids a direct dependency on the LayoutSystem
        ParseWarningSink disabledSink = ParseWarningSink::Disabled();
        sizedElementHandler_(registry, nestedShadowEntity, EntityHandle(registry, lightTarget),
                             targetEntity.value(), branchType, disabledSink);
      }

      // Set the sourceEntity as the element in the light tree (i.e. the original <use>)
      if (nestedShadowEntity != entt::null) {
        registry.emplace<ShadowTreeRootComponent>(nestedShadowEntity, lightTarget);
      }

      return shadow;
    } else {
      ParseDiagnostic err;
      err.reason = std::string("Failed to find target entity for nested shadow tree '") +
                   nestedShadow->mainHref().value_or("") + "'";
      warningSink.add(std::move(err));
      return entt::null;
    }
  } else {
    const Entity shadow =
        createShadowEntity(registry, branchType, storage, lightTarget, shadowParent);

    for (auto child = registry.get<donner::components::TreeComponent>(lightTarget).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      // The guard tracks href targets, not ordinary source-tree traversal. Adding a child here
      // makes a legal descent back through a referenced container look like a duplicate before
      // the nested href can be checked, and RecursionGuard::with() asserts. The nested-shadow path
      // above atomically checks and adds every followed target.
      std::ignore = createShadowAndChildren(registry, branchType, storage, guard, shadow, child,
                                            shadowHostParents, warningSink);
    }

    return shadow;
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg::components
