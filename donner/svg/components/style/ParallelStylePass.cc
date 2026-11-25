#include "donner/svg/components/style/ParallelStylePass.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "donner/base/SmallVector.h"
#include "donner/base/tasks/TaskScope.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg::components {
namespace {

using donner::components::TreeComponent;
using Batch = StyleSystem::PreparedBatch;
constexpr std::size_t kThreshold = 1000;
constexpr std::size_t kGrain = 64;
// Client working space above the scheduler's reporting reserve.
constexpr std::size_t kCascadeStackBytes = 64 * 1024;
constexpr std::size_t kCascadeHeadroom = kCascadeStackBytes + tasks::kStackReserveBytes;

bool HasCascadeStackHeadroom(const tasks::StackBounds& bounds,
                             std::size_t required = kCascadeHeadroom) {
  const auto remaining = tasks::StackRemaining(tasks::CurrentStackPosition(), bounds);
  return remaining >= required && remaining <= bounds.size;
}
using Index = std::uint32_t;
constexpr Index kEnd = std::numeric_limits<Index>::max();

enum class Phase { Unknown, Visiting, Serial, Main, Offscreen };

struct Node {
  Entity entity;
  Entity parentEntity;
  Index parent = kEnd;
  Index child = kEnd;
  Index next = kEnd;
  Index size = 1;
  Phase phase = Phase::Unknown;
};

bool HasRecord(const Registry& registry, Entity entity) {
  const auto* slot = registry.try_get<ComputedStyleComponent>(entity);
  return slot && slot->style;
}

struct Forest {
  std::vector<Entity> requests;
  std::vector<Entity> entities;
  std::vector<Node> nodes;
  entt::sparse_set indices;

  Index indexOf(Entity entity) const {
    return indices.contains(entity) ? static_cast<Index>(indices.index(entity)) : kEnd;
  }

  Index mainRoots = kEnd;
  Index offscreenRoots = kEnd;

  bool addRequest(const Registry& registry, Entity entity) {
    if (HasRecord(registry, entity)) {
      return true;
    }
    requests.push_back(entity);
    do {
      if (indices.contains(entity)) {
        break;
      }
      if (nodes.size() == Batch::kMaximumElements) {
        return false;
      }
      std::ignore = indices.push(entity);
      const Entity parent = registry.get<TreeComponent>(entity).parent();
      nodes.push_back({entity, parent});
      entities.push_back(entity);
      entity = parent;
    } while (entity != entt::null && !HasRecord(registry, entity));
    return true;
  }

  bool collect(const Registry& registry, StylePassSelection selection, std::size_t threshold) {
    const auto size = selection == StylePassSelection::Dirty
                          ? registry.view<const DirtyFlagsComponent>().size()
                          : registry.view<const TreeComponent>().size();
    const auto capacity = std::min<std::size_t>(size, Batch::kMaximumElements);
    nodes.reserve(capacity);
    requests.reserve(capacity);
    entities.reserve(capacity);
    indices.reserve(capacity);
    if (selection == StylePassSelection::Dirty) {
      const auto view = registry.view<const DirtyFlagsComponent>();
      for (Entity entity : view) {
        if (view.get<const DirtyFlagsComponent>(entity).test(DirtyFlagsComponent::Style) &&
            !addRequest(registry, entity)) {
          return false;
        }
      }
    } else {
      for (Entity entity : registry.view<const TreeComponent>()) {
        if (!addRequest(registry, entity)) {
          return false;
        }
      }
    }
    return nodes.size() >= threshold;
  }

  Phase& phaseOf(Entity entity, std::unordered_map<Entity, Phase>& outside) {
    const Index index = indexOf(entity);
    return index != kEnd ? nodes[index].phase : outside[entity];
  }

  bool classifyNode(Node& node, const Registry& registry,
                    std::unordered_map<Entity, Phase>& outside, std::vector<Phase*>& path) {
    path.clear();
    Entity entity = node.entity;
    Phase* phase = &node.phase;
    while (*phase == Phase::Unknown) {
      if (outside.size() > Batch::kMaximumElements + 2 - nodes.size()) {
        return false;
      }
      *phase = Phase::Visiting;
      path.push_back(phase);
      const Index known = indexOf(entity);
      entity =
          known == kEnd ? registry.get<TreeComponent>(entity).parent() : nodes[known].parentEntity;
      phase = &phaseOf(entity, outside);
    }
    if (*phase == Phase::Visiting) {
      return false;
    }
    for (Phase* visited : path) {
      *visited = *phase;
    }
    return true;
  }

  bool identifyPhases(const Registry& registry) {
    const auto* document = registry.ctx().find<SVGDocumentContext>();
    if (!document || !registry.valid(document->rootEntity)) {
      return false;
    }
    // Collected entities already have a dense slot. Only retained ancestors outside the
    // requested set need a separate entry when classifying a selective pass.
    std::unordered_map<Entity, Phase> outside;
    phaseOf(entt::null, outside) = Phase::Serial;
    phaseOf(document->rootEntity, outside) = Phase::Main;
    const auto shadows = registry.view<const ComputedShadowTreeComponent>();
    for (Entity host : shadows) {
      const auto& tree = shadows.get<const ComputedShadowTreeComponent>(host);
      for (std::size_t i = 0; i < tree.offscreenShadowCount(); ++i) {
        const Entity root = tree.offscreenShadowRoot(i);
        if (root != entt::null) {
          phaseOf(root, outside) = Phase::Offscreen;
        }
      }
    }
    std::vector<Phase*> path;
    for (Node& node : nodes) {
      if (!classifyNode(node, registry, outside, path)) {
        return false;
      }
    }
    return true;
  }

  std::size_t linkScheduledNodes() {
    std::size_t scheduled = 0;
    for (std::size_t i = nodes.size(); i-- != 0;) {
      Node& node = nodes[i];
      if (node.phase == Phase::Serial) {
        continue;
      }
      ++scheduled;
      const Index parent = indexOf(node.parentEntity);
      if (parent != kEnd && nodes[parent].phase == node.phase) {
        node.parent = parent;
      }
      Index& first = node.parent != kEnd              ? nodes[node.parent].child
                     : node.phase == Phase::Offscreen ? offscreenRoots
                                                      : mainRoots;
      node.next = first;
      first = static_cast<Index>(i);
    }

    return scheduled;
  }

  bool connect(const Registry& registry, std::size_t threshold) {
    if (!identifyPhases(registry)) {
      return false;
    }
    const auto scheduled = linkScheduledNodes();
    std::vector<Index> order;
    order.reserve(nodes.size());
    for (auto first : {mainRoots, offscreenRoots}) {
      for (auto root = first; root != kEnd; root = nodes[root].next) {
        order.push_back(root);
      }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
      for (Index child = nodes[order[i]].child; child != kEnd; child = nodes[child].next) {
        order.push_back(child);
      }
    }
    if (order.size() != scheduled || scheduled < threshold) {
      return false;
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      auto& node = nodes[*it];
      if (node.parent != kEnd) {
        nodes[node.parent].size += node.size;
      }
    }
    return true;
  }
};

#ifndef NDEBUG
auto PoolSizes(const Registry& registry) {
  std::vector<std::pair<entt::id_type, std::size_t>> sizes;
  for (auto [key, pool] : registry.storage()) {
    sizes.emplace_back(key, pool.size());
  }
  const auto* entities = registry.storage<Entity>();
  return std::tuple{entities->size(), entities->free_list(), sizes};
}
#endif

struct Range {
  Index first;
  Index end;
};

using RangeStack = SmallVector<Range, 16>;

class Traversal {
public:
  Traversal(tasks::TaskScope& scope, const Forest& forest, Batch& batch,
            const StylePassPhasesForTesting* hooks, tasks::StackBounds ownerStack)
      : scope_(scope),
        forest_(forest),
        batch_(batch),
        hooks_(hooks),
        ownerThread_(std::this_thread::get_id()),
        ownerStack_(ownerStack),
        requiredStackHeadroom_(hooks ? std::max(kCascadeHeadroom, hooks->rangeStackHeadroom)
                                     : kCascadeHeadroom),
        grain_(hooks ? std::max<std::size_t>(1, hooks->grain) : kGrain) {}

  void start(std::size_t workers) {
    const Range initial{forest_.mainRoots, kEnd};
    if (workers == 0) {
      launch(initial, true);
    } else {
      // The document owner can execute its own ready ranges before joining the other workers.
      run(initial, true, forest_.nodes.size() / (workers + 1));
    }
  }

  std::size_t admitted() const { return admitted_.load(std::memory_order_relaxed); }
  std::size_t declined() const { return declined_.load(std::memory_order_relaxed); }

  bool completed() const { return !aborted_.load(std::memory_order_relaxed); }

private:
  bool enqueue(Range range, bool main) {
    if (scope_.trySpawn([this, range, main] { run(range, main); })) {
      admitted_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    declined_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  void launch(Range range, bool main) {
    if (!enqueue(range, main)) {
      run(range, main);
    }
  }

  void offer(Range range, bool main, RangeStack& local) {
    if (main) {
      outstandingMain_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!enqueue(range, main)) {
      if (main) {
        outstandingMain_.fetch_sub(1, std::memory_order_relaxed);
      }
      local.push_back(range);
    }
  }

  void split(Range range, bool main, RangeStack& local, std::size_t& ownerWork) {
    Index group = range.first;
    Index work = 0;
    for (Index i = group; i != range.end; i = forest_.nodes[i].next) {
      const auto& node = forest_.nodes[i];
      work += node.size;
      if (work >= grain_ && node.next != range.end) {
        if (ownerWork != 0) {
          local.push_back({group, node.next});
          ownerWork -= std::min<std::size_t>(ownerWork, work);
        } else {
          offer({group, node.next}, main, local);
        }
        group = node.next;
        work = 0;
      }
    }
    local.push_back({group, range.end});
  }

  bool hasRangeStack() const {
    if (std::this_thread::get_id() == ownerThread_) {
      return HasCascadeStackHeadroom(ownerStack_, requiredStackHeadroom_);
    }
    const auto stack = tasks::TaskScheduler::TryCurrentThreadUsableStack();
    return stack && HasCascadeStackHeadroom(*stack, requiredStackHeadroom_);
  }

  void run(Range range, bool main, std::size_t ownerWork = 0) {
    auto* worker = hasRangeStack() ? batch_.bindWorker() : nullptr;
    if (worker) {
      walk(range, main, *worker, ownerWork);
    } else {
      aborted_.store(true, std::memory_order_relaxed);
    }
    // The last main task publishes all completed main-tree records before offscreen roots start.
    if (main && outstandingMain_.fetch_sub(1, std::memory_order_acq_rel) == 1 && completed() &&
        forest_.offscreenRoots != kEnd) {
      if (hooks_ && hooks_->beforeOffscreen) {
        hooks_->beforeOffscreen();
      }
      const Range offscreen{forest_.offscreenRoots, kEnd};
      if (!enqueue(offscreen, false)) {
        // The main walk has returned. Reuse this context without nesting another range frame.
        UTILS_RELEASE_ASSERT(worker);
        walk(offscreen, false, *worker, 0);
      }
    }
  }

  bool publish(Index index, const Node& node, Batch::Worker& worker) {
    if (!batch_.computeAt(index, node.entity, worker)) {
      return false;
    }
    if (hooks_ && hooks_->published) {
      hooks_->published(node.entity);
    }
    return true;
  }

  bool children(const Node& node, bool main, RangeStack& local, std::size_t& ownerWork) {
    if (node.child == kEnd) {
      return true;
    }
    if (hooks_ && hooks_->allowChildAdmission && !hooks_->allowChildAdmission(node.entity)) {
      return false;
    }
    split({node.child, kEnd}, main, local, ownerWork);
    return true;
  }

  void walk(Range range, bool main, Batch::Worker& worker, std::size_t ownerWork) {
    RangeStack local;
    split(range, main, local, ownerWork);
    while (!local.empty() && completed()) {
      const Range current = local.back();
      local.pop_back();
      for (Index i = current.first; i != current.end; i = forest_.nodes[i].next) {
        const auto& node = forest_.nodes[i];
        if (hooks_ && hooks_->beforeNode) {
          hooks_->beforeNode(node.entity);
        }
        if (!publish(i, node, worker) || !children(node, main, local, ownerWork)) {
          aborted_.store(true, std::memory_order_relaxed);
          return;
        }
      }
    }
  }

  tasks::TaskScope& scope_;
  const Forest& forest_;
  Batch& batch_;
  const StylePassPhasesForTesting* hooks_;
  const std::thread::id ownerThread_;
  const tasks::StackBounds ownerStack_;
  const std::size_t requiredStackHeadroom_;
  const std::size_t grain_;
  std::atomic<std::size_t> outstandingMain_{1};
  std::atomic<bool> aborted_{false};
  std::atomic<std::size_t> admitted_{0};
  std::atomic<std::size_t> declined_{0};
};

bool CanEnterStyleScope(const tasks::TaskScheduler* scheduler) {
  // The owner holds document access, which an unrelated task could need.
#if defined(__APPLE__) || defined(__linux__)
  return scheduler && scheduler->canJoinWithoutRunningUnrelatedTasks() &&
         scheduler->schedulingWidth() != 0 &&
         (scheduler->workerCount() == 0 || scheduler->workerStackBytes() >= kCascadeHeadroom);
#else
  (void)scheduler;
  return false;
#endif
}

bool HasShallowStylesheets(const Registry& registry) {
  const auto sheets = registry.view<const StylesheetComponent>();
  for (Entity entity : sheets) {
    const auto& sheet = sheets.get<const StylesheetComponent>(entity);
    if (!HasSvgNamespace(registry, entity) || !sheet.isCssType()) {
      continue;
    }
    if (!sheet.ruleIndex.shallowForParallelCascade() ||
        !std::ranges::all_of(sheet.importedRuleIndexes, &RuleIndex::shallowForParallelCascade)) {
      return false;
    }
  }
  return true;
}

bool Eligible(const Registry& registry, std::size_t threshold) {
  if (registry.view<const TreeComponent>().size() < threshold ||
      registry.ctx().contains<ExternalUseStyleContext>() ||
      !registry.view<const AnimatedValuesComponent>().empty()) {
    return false;
  }
  const auto* budget = registry.ctx().find<StyleResourceBudget>();
  return (!budget || (!budget->rejected() && budget->retainedStyleBytes()->canReserve(0))) &&
         HasShallowStylesheets(registry);
}

bool FrontierParentsReady(const Forest& forest, const Registry& registry) {
  for (const Node& node : forest.nodes) {
    if (node.phase == Phase::Serial || node.parent != kEnd) {
      continue;
    }
    const Entity parent = node.parentEntity;
    const Index found = forest.indexOf(parent);
    const bool waitsForMain =
        node.phase == Phase::Offscreen && found != kEnd && forest.nodes[found].phase == Phase::Main;
    if (parent != entt::null && !waitsForMain && !HasRecord(registry, parent)) {
      return false;
    }
  }
  return true;
}

std::optional<tasks::StackBounds> PrepareForest(Forest& forest, const Registry& registry,
                                                StylePassSelection selection,
                                                std::size_t threshold) {
  if (!forest.collect(registry, selection, threshold) || !forest.connect(registry, threshold)) {
    return std::nullopt;
  }
  const auto stack = tasks::TaskScheduler::TryCurrentThreadUsableStack();
  return stack && HasCascadeStackHeadroom(*stack) ? stack : std::nullopt;
}

bool RunTraversal(tasks::TaskScheduler& scheduler, const Forest& forest, Batch& batch,
                  StyleCascadeMetrics& metrics, const StylePassPhasesForTesting* hooks,
                  tasks::StackBounds ownerStack) {
  if (hooks && hooks->beforeRegion) {
    hooks->beforeRegion();
  }
  tasks::TaskScope scope(scheduler);
  Traversal traversal(scope, forest, batch, hooks, ownerStack);
  traversal.start(scheduler.workerCount());
  scope.wait();
  ++metrics.scheduledPasses;
  metrics.admittedStyleTasks += traversal.admitted();
  metrics.declinedStyleTasks += traversal.declined();
  return traversal.completed();
}
}  // namespace

bool TryParallelStylePass(Registry& registry, ParseWarningSink& warnings,
                          tasks::TaskScheduler* scheduler, StylePassSelection selection) {
  if (!CanEnterStyleScope(scheduler)) {
    return false;
  }
  const auto* hooks = registry.ctx().find<StylePassPhasesForTesting>();
  const auto threshold = hooks ? std::max<std::size_t>(1, hooks->minimumElements) : kThreshold;
  if (!Eligible(registry, threshold)) {
    return false;
  }
  Forest forest;
  const auto ownerStack = PrepareForest(forest, registry, selection, threshold);
  if (!ownerStack) {
    return false;
  }
  for (Entity entity : forest.entities) {
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }
  Batch batch(registry, forest.entities, scheduler->workerCount() + 1, warnings.isEnabled(), true);
  auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
#ifndef NDEBUG
  const auto sizes = PoolSizes(registry);
#endif
  const bool completed = batch.prepareDocumentRoot() && FrontierParentsReady(forest, registry) &&
                         RunTraversal(*scheduler, forest, batch, metrics, hooks, *ownerStack);
#ifndef NDEBUG
  assert(sizes == PoolSizes(registry));
#endif
  if (hooks && hooks->beforeCommit) {
    hooks->beforeCommit();
  }
  metrics.abandonedStylePasses += !completed;
  metrics.replayedSelectorTraces += batch.commit(forest.requests, warnings, completed);
  return true;
}

}  // namespace donner::svg::components

namespace donner::svg {
StylePassScheduler::StylePassScheduler(tasks::TaskScheduler* scheduler)
    : scheduler_(scheduler), dispatch_(scheduler ? components::TryParallelStylePass : nullptr) {}
}  // namespace donner::svg
