#include "donner/svg/compositor/MandatoryHintDetector.h"

#include <unordered_set>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg::compositor {

bool MandatoryHintDetector::qualifies(const components::RenderingInstanceComponent& instance,
                                      MandatoryHintScope scope) {
  // Three signals force isolated compositing:
  //   - isolatedLayer: subsumes opacity < 1, mix-blend-mode != normal, isolation: isolate
  //   - resolvedFilter.has_value(): `filter` applied
  //   - mask.has_value(): `mask` applied
  // FilterOnly narrows to the filter signal; the others render through the
  // driver's inline isolation path instead of a retained layer.
  if (scope == MandatoryHintScope::FilterOnly) {
    return instance.resolvedFilter.has_value();
  }
  return instance.isolatedLayer || instance.resolvedFilter.has_value() || instance.mask.has_value();
}

namespace {

/// Returns true if any ancestor of @p entity carries a compositing context
/// that would be broken if the descendant were extracted into its own cached
/// layer - the ancestor's context is applied by the main tree walk, not
/// replayed when the extracted layer bitmap is composited back. Returning
/// true is a signal to NOT auto-promote.
///
/// Under `All`, clip-path / mask / filter ancestors qualify; an isolation
/// ancestor (opacity < 1, blend mode) does not need to, because it is itself
/// promoted. Under `FilterOnly`, isolation ancestors stay INLINE (their
/// signal is out of scope), so extracting a descendant filter would split
/// the group: trailing in-group content would land in a segment slice that
/// re-enters the group at full alpha, and the tile blit resets paint before
/// composing. Treat unhinted isolation ancestors as breaking too; the nested
/// filter then renders fully inline - pixel-correct, merely uncached.
bool HasCompositingBreakingAncestor(Registry& registry, Entity entity, MandatoryHintScope scope) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
  if (tree == nullptr) {
    return false;
  }
  Entity cursor = tree->parent();
  while (cursor != entt::null && registry.valid(cursor)) {
    if (const auto* ancestorInstance =
            registry.try_get<components::RenderingInstanceComponent>(cursor)) {
      if (ancestorInstance->clipPath.has_value() || ancestorInstance->mask.has_value() ||
          ancestorInstance->resolvedFilter.has_value()) {
        return true;
      }
      if (scope == MandatoryHintScope::FilterOnly && ancestorInstance->isolatedLayer) {
        return true;
      }
    }
    const auto* ancestorTree = registry.try_get<donner::components::TreeComponent>(cursor);
    if (ancestorTree == nullptr) {
      break;
    }
    cursor = ancestorTree->parent();
  }
  return false;
}

}  // namespace

void MandatoryHintDetector::reconcile(Registry& registry) {
  stats_ = {};

  // Snapshot the entities that currently qualify.
  std::unordered_set<Entity> qualifying;
  auto view = registry.view<components::RenderingInstanceComponent>();
  for (auto entity : view) {
    ++stats_.candidatesEvaluated;
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (!qualifies(instance, scope_)) {
      continue;
    }
    // Don't auto-promote if an ancestor's clip-path / mask / filter would be
    // lost by extracting the subtree into its own cached layer. Such entities
    // fall through to the driver's inline `pushClip` / `pushMask` /
    // `pushFilterLayer` path, which handles the context correctly.
    if (HasCompositingBreakingAncestor(registry, entity, scope_)) {
      continue;
    }
    qualifying.insert(entity);
  }

  // Drop hints for entities that no longer qualify, or whose entity is no
  // longer valid (destroyed between reconciles).
  std::vector<Entity> stale;
  stale.reserve(hints_.size());
  for (const auto& [entity, hint] : hints_) {
    if (!registry.valid(entity) || qualifying.count(entity) == 0) {
      stale.push_back(entity);
    }
  }
  for (Entity entity : stale) {
    hints_.erase(entity);
    ++stats_.hintsDropped;
  }

  // Publish hints for newly-qualifying entities.
  for (Entity entity : qualifying) {
    if (hints_.count(entity) == 0) {
      hints_.emplace(entity, ScopedCompositorHint::Mandatory(registry, entity));
      ++stats_.hintsPublished;
    }
  }

  stats_.hintsActive = static_cast<uint32_t>(hints_.size());
}

}  // namespace donner::svg::compositor
