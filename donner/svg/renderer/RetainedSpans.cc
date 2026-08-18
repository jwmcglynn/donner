#include "donner/svg/renderer/RetainedSpans.h"

#include <algorithm>
#include <vector>

#include "donner/svg/components/shape/ComputedPathComponent.h"

namespace donner::svg {

namespace {

/// Marks a registry as already carrying the invalidation listener. Lives in the registry's
/// context, so it goes away with the registry: a later registry allocated at the same address
/// correctly misses it and gets its own connection. Pointer identity alone cannot tell those
/// two cases apart.
struct RetainedSpanListenerInstalled {};

bool TransformsEqual(const Transform2d& lhs, const Transform2d& rhs) {
  return std::equal(std::begin(lhs.data), std::end(lhs.data), std::begin(rhs.data));
}

/// Drops an entity's retained coverage and returns the bytes it was charged.
std::size_t DropRetainedEntry(Registry& registry, Entity entity) {
  auto* entry = registry.try_get<RetainedSpansComponent>(entity);
  if (entry == nullptr) {
    return 0;
  }

  const std::size_t bytes = entry->chargedBytes();
  registry.remove<RetainedSpansComponent>(entity);
  return bytes;
}

void ReleaseBytes(RetainedSpanBudget& budget, std::size_t bytes) {
  budget.liveBytes -= std::min(budget.liveBytes, bytes);
}

/// Drops an entity's retained coverage when its resolved path changes or goes away.
///
/// The resolved-path signals only fire when the geometry actually differs, so an idle
/// re-render leaves retained coverage intact, and any real outline change removes it before the
/// next draw can replay it.
void OnComputedPathChanged(Registry& registry, Entity entity) {
  const std::size_t bytes = DropRetainedEntry(registry, entity);
  if (bytes == 0) {
    return;
  }

  if (auto* budget = registry.ctx().find<RetainedSpanBudget>()) {
    ReleaseBytes(*budget, bytes);
  }
}

}  // namespace

bool operator==(const RetainedSpanKey& lhs, const RetainedSpanKey& rhs) {
  return TransformsEqual(lhs.deviceFromLocal, rhs.deviceFromLocal) && lhs.paint == rhs.paint &&
         lhs.stroke == rhs.stroke && lhs.clipEpoch == rhs.clipEpoch &&
         lhs.surfaceSize == rhs.surfaceSize && lhs.fillRule == rhs.fillRule &&
         lhs.openDashSeam == rhs.openDashSeam;
}

void EnsureRetainedSpanInvalidationWired(Registry& registry) {
  if (registry.ctx().contains<RetainedSpanListenerInstalled>()) {
    return;
  }

  registry.ctx().emplace<RetainedSpanListenerInstalled>();
  // Materialize the entry pool while the registry is quiescent. The listener below reaches it on
  // every resolved-path change, including for entities that never retained anything, and a pool
  // created for the first time from inside a destruction signal is a situation not worth having
  // to reason about.
  static_cast<void>(registry.storage<RetainedSpansComponent>());
  registry.on_update<components::ComputedPathComponent>().connect<&OnComputedPathChanged>();
  registry.on_destroy<components::ComputedPathComponent>().connect<&OnComputedPathChanged>();
  // The connection outlives the renderer on purpose: the listener is a free function with no
  // captured state, and it dies with the registry it is attached to. It shares these two
  // signals with the conversion caches' listener; entt calls both, and each drops only its own
  // component.
}

RetainedSpanBudget& RetainedSpanBudgetFor(Registry& registry, std::size_t budgetBytes) {
  RetainedSpanBudget& budget = registry.ctx().get_or_emplace<RetainedSpanBudget>();
  if (budget.budgetBytes != budgetBytes) {
    // A new budget is a new answer to "does the working set fit", so a document that gave up
    // under the old one gets to try again.
    budget.budgetBytes = budgetBytes;
    budget.disabled = false;
  }
  return budget;
}

void ClearRetainedSpans(Registry& registry) {
  registry.clear<RetainedSpansComponent>();
  if (auto* budget = registry.ctx().find<RetainedSpanBudget>()) {
    budget->liveBytes = 0;
  }
}

void EvictRetainedSpansToBudget(Registry& registry, std::uint64_t currentFrame) {
  auto* budget = registry.ctx().find<RetainedSpanBudget>();
  if (budget == nullptr || budget->liveBytes <= budget->budgetBytes) {
    return;
  }

  // Coldest first: an entry not drawn this frame costs nothing to lose right now, while an
  // entry drawn this frame would be re-captured immediately.
  struct Candidate {
    Entity entity;
    std::uint64_t lastUsedFrame;
    std::size_t bytes;
  };

  std::vector<Candidate> candidates;
  const auto view = registry.view<const RetainedSpansComponent>();
  candidates.reserve(view.size());
  for (const Entity entity : view) {
    const RetainedSpansComponent& entry = view.get<const RetainedSpansComponent>(entity);
    if (entry.lastUsedFrame >= currentFrame) {
      continue;
    }
    candidates.push_back(Candidate{entity, entry.lastUsedFrame, entry.chargedBytes()});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) {
              return lhs.lastUsedFrame < rhs.lastUsedFrame;
            });

  for (const Candidate& candidate : candidates) {
    if (budget->liveBytes <= budget->budgetBytes) {
      return;
    }

    DropRetainedEntry(registry, candidate.entity);
    ReleaseBytes(*budget, candidate.bytes);
    ++budget->evictions;
  }

  if (budget->liveBytes <= budget->budgetBytes) {
    return;
  }

  // Every remaining entry was drawn this frame, so the working set itself does not fit. Keeping
  // any of it would evict and re-capture the same shapes every frame, which costs more than not
  // retaining at all.
  ClearRetainedSpans(registry);
  budget->disabled = true;
}

}  // namespace donner::svg
