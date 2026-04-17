#include "donner/svg/compositor/LayerResolver.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

#include "donner/svg/compositor/CompositorHintComponent.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"

namespace donner::svg::compositor {

namespace {

struct Candidate {
  Entity entity;
  uint32_t totalWeight;
  bool mandatory;
};

}  // namespace

void LayerResolver::resolve(Registry& registry, uint32_t maxLayers) {
  stats_ = {};

  // Clean up stale ComputedLayerAssignmentComponents on entities that no longer have any hints.
  // Collect first (we can't modify storage while iterating a view), then remove.
  {
    std::vector<Entity> stale;
    auto assignmentView = registry.view<ComputedLayerAssignmentComponent>();
    for (auto entity : assignmentView) {
      if (!registry.all_of<CompositorHintComponent>(entity)) {
        stale.push_back(entity);
      }
    }
    for (Entity entity : stale) {
      registry.remove<ComputedLayerAssignmentComponent>(entity);
    }
  }

  // Collect candidates. An entity qualifies when it has at least one hint entry.
  std::vector<Candidate> mandatoryCandidates;
  std::vector<Candidate> contestedCandidates;

  auto view = registry.view<CompositorHintComponent>();
  for (auto entity : view) {
    const auto& hint = view.get<CompositorHintComponent>(entity);
    if (hint.empty()) {
      continue;
    }
    ++stats_.candidatesEvaluated;

    Candidate candidate;
    candidate.entity = entity;
    candidate.totalWeight = hint.totalWeight();
    candidate.mandatory = (candidate.totalWeight == std::numeric_limits<uint32_t>::max());
    if (candidate.mandatory) {
      mandatoryCandidates.push_back(candidate);
    } else {
      contestedCandidates.push_back(candidate);
    }
  }

  // Sort mandatory candidates deterministically by entity id ascending. All of
  // them take layers as long as budget holds; over-budget mandatory sets both
  // log and count toward `budgetExhaustions` so tests can detect the condition.
  std::sort(mandatoryCandidates.begin(), mandatoryCandidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return static_cast<std::uint32_t>(a.entity) <
                     static_cast<std::uint32_t>(b.entity);
            });

  // Sort contested candidates by weight descending, ties broken by entity id ascending.
  // TODO(phase 2): break ties by draw order once RenderingInstanceComponent participates.
  std::sort(contestedCandidates.begin(), contestedCandidates.end(),
            [](const Candidate& a, const Candidate& b) {
              if (a.totalWeight != b.totalWeight) {
                return a.totalWeight > b.totalWeight;
              }
              return static_cast<std::uint32_t>(a.entity) <
                     static_cast<std::uint32_t>(b.entity);
            });

  // Assignment pass.
  uint32_t nextLayerId = 1;
  uint32_t budgetRemaining = maxLayers;

  auto assignOrSkip = [&](Entity entity) {
    if (budgetRemaining == 0) {
      // Lost to the budget cap. Ensure no stale assignment lingers.
      registry.remove<ComputedLayerAssignmentComponent>(entity);
      return false;
    }
    const uint32_t layerId = nextLayerId++;
    --budgetRemaining;

    auto* existing = registry.try_get<ComputedLayerAssignmentComponent>(entity);
    if (existing == nullptr) {
      registry.emplace<ComputedLayerAssignmentComponent>(entity,
                                                         ComputedLayerAssignmentComponent{layerId});
    } else if (existing->layerId != layerId) {
      existing->layerId = layerId;
    }
    ++stats_.layersAssigned;
    return true;
  };

  for (const Candidate& candidate : mandatoryCandidates) {
    if (budgetRemaining == 0) {
      // Over-budget mandatory candidate. Design doc: "log/TODO for budget exhaustion".
      std::fprintf(stderr,
                   "LayerResolver: mandatory candidate %u exceeds maxLayers=%u; "
                   "evicting from layer assignment.\n",
                   static_cast<uint32_t>(candidate.entity), maxLayers);
      ++stats_.budgetExhaustions;
      registry.remove<ComputedLayerAssignmentComponent>(candidate.entity);
      continue;
    }
    assignOrSkip(candidate.entity);
  }

  for (const Candidate& candidate : contestedCandidates) {
    if (budgetRemaining == 0) {
      ++stats_.budgetExhaustions;
      registry.remove<ComputedLayerAssignmentComponent>(candidate.entity);
      continue;
    }
    assignOrSkip(candidate.entity);
  }
}

}  // namespace donner::svg::compositor
