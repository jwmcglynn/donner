#include "donner/svg/compositor/MandatoryHintDetector.h"

#include <unordered_set>
#include <vector>

#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg::compositor {

bool MandatoryHintDetector::qualifies(const components::RenderingInstanceComponent& instance) {
  // Three signals per the design doc's "Mandatory hints" table:
  //   - isolatedLayer: subsumes opacity < 1, mix-blend-mode != normal, isolation: isolate
  //   - resolvedFilter.has_value(): `filter` applied
  //   - mask.has_value(): `mask` applied
  return instance.isolatedLayer || instance.resolvedFilter.has_value() || instance.mask.has_value();
}

void MandatoryHintDetector::reconcile(Registry& registry) {
  stats_ = {};

  // Snapshot the entities that currently qualify.
  std::unordered_set<Entity> qualifying;
  auto view = registry.view<components::RenderingInstanceComponent>();
  for (auto entity : view) {
    ++stats_.candidatesEvaluated;
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (qualifies(instance)) {
      qualifying.insert(entity);
    }
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
