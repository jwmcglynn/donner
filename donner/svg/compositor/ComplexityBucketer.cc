#include "donner/svg/compositor/ComplexityBucketer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg::compositor {

namespace {

struct Candidate {
  Entity entity;
  uint64_t cost;
};

}  // namespace

void ComplexityBucketer::reconcile(Registry& registry) {
  stats_ = {};

  // Collect entities in draw order. `RenderingInstanceView`'s native iteration
  // is reverse-insertion-order (that's how `entt::storage::begin()` behaves);
  // we drain it into a vector, then reverse so the first entry is the root
  // and children follow in draw order.
  std::vector<Entity> drawOrder;
  {
    RenderingInstanceView view(registry);
    while (!view.done()) {
      drawOrder.push_back(view.currentEntity());
      view.advance();
    }
  }
  std::reverse(drawOrder.begin(), drawOrder.end());

  if (drawOrder.empty()) {
    // Empty document — drop any lingering hints and exit.
    const uint32_t droppedCount = static_cast<uint32_t>(bucketHints_.size());
    bucketHints_.clear();
    stats_.bucketsDropped = droppedCount;
    return;
  }

  // The first entity (in draw order) is the document root. Skip it as a
  // candidate; the root would never benefit from being its own bucket.
  ++stats_.entitiesEvaluated;

  std::vector<Candidate> candidates;
  size_t i = 1;
  while (i < drawOrder.size()) {
    const Entity childRoot = drawOrder[i];
    const auto& childInstance = registry.get<components::RenderingInstanceComponent>(childRoot);
    const Entity subtreeEnd = childInstance.subtreeInfo.has_value()
                                  ? childInstance.subtreeInfo->lastRenderedEntity
                                  : childRoot;

    uint64_t cost = 0;
    while (i < drawOrder.size()) {
      const Entity current = drawOrder[i];
      const auto& instance = registry.get<components::RenderingInstanceComponent>(current);
      cost += 1;
      if (instance.resolvedFilter.has_value()) {
        cost += config_.filterPenalty;
      }
      if (instance.mask.has_value()) {
        cost += config_.maskPenalty;
      }
      ++stats_.entitiesEvaluated;

      ++i;
      if (current == subtreeEnd) {
        break;
      }
    }

    // Skip candidates whose subtree cost is below the bucketing threshold.
    // Cheap subtrees (a lone `<rect>` fill background) aren't worth their own
    // layer; carving them out also exposes correctness gaps in
    // `RendererDriver::drawEntityRange` for standalone top-level elements.
    if (cost < config_.minCostToBucket) {
      continue;
    }

    candidates.push_back(Candidate{childRoot, cost});
    ++stats_.candidatesConsidered;
  }

  // Sort: cost descending, ties broken by entity id ascending (deterministic).
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.cost != b.cost) {
      return a.cost > b.cost;
    }
    return static_cast<uint32_t>(a.entity) < static_cast<uint32_t>(b.entity);
  });

  // Effective bucket count: budget minus reserved slots, floored at 0.
  const uint32_t budget = config_.targetBucketCount > config_.reservedSlots
                              ? config_.targetBucketCount - config_.reservedSlots
                              : 0;

  // Build the winning set (by entity).
  std::vector<Entity> winners;
  winners.reserve(budget);
  for (const Candidate& c : candidates) {
    if (winners.size() >= budget) {
      break;
    }
    winners.push_back(c.entity);
  }

  // Drop hints for entities that are no longer winners or whose entity is
  // gone.
  std::vector<Entity> stale;
  stale.reserve(bucketHints_.size());
  for (const auto& [entity, hint] : bucketHints_) {
    const bool invalid = !registry.valid(entity);
    const bool evicted =
        std::find(winners.begin(), winners.end(), entity) == winners.end();
    if (invalid || evicted) {
      stale.push_back(entity);
    }
  }
  for (Entity entity : stale) {
    bucketHints_.erase(entity);
    ++stats_.bucketsDropped;
  }

  // Publish hints for newly-winning entities.
  for (Entity entity : winners) {
    if (bucketHints_.count(entity) == 0) {
      bucketHints_.emplace(entity, ScopedCompositorHint(registry, entity,
                                                         HintSource::ComplexityBucket, 0x4000));
      ++stats_.bucketsPublished;
    }
  }

  stats_.bucketsActive = static_cast<uint32_t>(bucketHints_.size());
}

}  // namespace donner::svg::compositor
