#include "donner/svg/components/NodeLifetimeCollector.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg::components {
namespace {

SVGDocumentContext* FindDocumentContext(Registry& registry) {
  return registry.ctx().find<SVGDocumentContext>();
}

void AppendPostOrder(Registry& registry, Entity root, std::vector<Entity>& out) {
  if (!registry.valid(root)) {
    return;
  }

  const auto* tree = registry.try_get<donner::components::TreeComponent>(root);
  if (tree != nullptr) {
    for (Entity child = tree->firstChild(); child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      AppendPostOrder(registry, child, out);
    }
  }

  out.push_back(root);
}

std::uint32_t MaxPublicHandlesInSubtree(Registry& registry, Entity root) {
  if (!registry.valid(root)) {
    return 0;
  }

  std::uint32_t maxPublicHandles = 0;
  donner::components::ForAllChildrenRecursive(EntityHandle(registry, root), [&](EntityHandle node) {
    if (const auto* lifetime = node.try_get<NodeLifetimeComponent>()) {
      maxPublicHandles = std::max(maxPublicHandles, lifetime->externalRefCount());
    }
  });
  return maxPublicHandles;
}

bool IsCollectibleDetachedRoot(Registry& registry, Entity root) {
  if (!registry.valid(root)) {
    return false;
  }

  const auto* lifetime = registry.try_get<NodeLifetimeComponent>(root);
  return lifetime != nullptr && !lifetime->isAttached() &&
         MaxPublicHandlesInSubtree(registry, root) == 0;
}

}  // namespace

void NodeLifetimeCollector::EnqueueDetachedRoot(Registry& registry, Entity detachedRoot) {
  if (detachedRoot == entt::null || !registry.valid(detachedRoot)) {
    return;
  }

  auto* context = FindDocumentContext(registry);
  if (context == nullptr) {
    return;
  }

  context->detachedNodeState().queueDetachedRoot(detachedRoot);
}

DetachedNodeDiagnostics NodeLifetimeCollector::Diagnostics(Registry& registry) {
  auto* context = FindDocumentContext(registry);
  if (context == nullptr) {
    return DetachedNodeDiagnostics();
  }

  const DetachedNodeState& detachedNodeState = context->detachedNodeState();
  DetachedNodeDiagnostics result;
  result.queuedDetachedRoots = detachedNodeState.detachedRoots.size();
  for (Entity detachedRoot : detachedNodeState.detachedRoots) {
    const std::uint32_t publicHandles = MaxPublicHandlesInSubtree(registry, detachedRoot);
    if (publicHandles > 0) {
      ++result.retainedByPublicHandles;
      result.maxPublicHandlesOnRetainedRoot =
          std::max(result.maxPublicHandlesOnRetainedRoot, publicHandles);
    }
  }
  result.retainedByPublicHandlesInLastPass = detachedNodeState.lastRetainedByPublicHandles.size();
  result.retainedBySnapshotOrObserverEpochs =
      detachedNodeState.lastRetainedBySnapshotOrObserverEpochs.size();
  result.maxRetainedSnapshotOrObserverEpoch = detachedNodeState.maxRetainedSnapshotOrObserverEpoch;
  result.collectedInLastPass = detachedNodeState.lastCollectedRoots.size();
  result.isCollecting = detachedNodeState.isCollecting;
  return result;
}

void NodeLifetimeCollector::Collect(Registry& registry) {
  auto* context = FindDocumentContext(registry);
  if (context == nullptr) {
    return;
  }

  DetachedNodeState& detachedNodeState = context->detachedNodeState();
  if (detachedNodeState.isCollecting) {
    return;
  }

  const bool collectionDeferred = context->hasActiveDetachedNodeCollectionDeferral();
  const std::uint64_t retainingEpoch =
      collectionDeferred ? context->activeDetachedNodeCollectionEpoch() : 0;

  detachedNodeState.isCollecting = true;
  detachedNodeState.lastCollectedRoots.clear();
  detachedNodeState.lastRetainedByPublicHandles.clear();
  detachedNodeState.lastRetainedBySnapshotOrObserverEpochs.clear();
  detachedNodeState.maxRetainedSnapshotOrObserverEpoch = 0;

  std::vector<Entity> remainingRoots;
  remainingRoots.reserve(detachedNodeState.detachedRoots.size());

  for (Entity root : detachedNodeState.detachedRoots) {
    if (root == entt::null || !registry.valid(root)) {
      continue;
    }

    const auto* lifetime = registry.try_get<NodeLifetimeComponent>(root);
    if (lifetime == nullptr || lifetime->isAttached()) {
      continue;
    }

    if (collectionDeferred) {
      detachedNodeState.lastRetainedBySnapshotOrObserverEpochs.push_back(root);
      detachedNodeState.maxRetainedSnapshotOrObserverEpoch =
          std::max(detachedNodeState.maxRetainedSnapshotOrObserverEpoch, retainingEpoch);
      remainingRoots.push_back(root);
      continue;
    }

    if (!IsCollectibleDetachedRoot(registry, root)) {
      detachedNodeState.lastRetainedByPublicHandles.push_back(root);
      remainingRoots.push_back(root);
      continue;
    }

    detachedNodeState.lastCollectedRoots.push_back(root);
    std::vector<Entity> destroyOrder;
    AppendPostOrder(registry, root, destroyOrder);
    for (Entity entity : destroyOrder) {
      if (!registry.valid(entity)) {
        continue;
      }

      if (auto* nodeLifetime = registry.try_get<NodeLifetimeComponent>(entity)) {
        nodeLifetime->markDestroying();
      }
      registry.destroy(entity);
    }
  }

  detachedNodeState.detachedRoots = std::move(remainingRoots);
  detachedNodeState.isCollecting = false;
}

}  // namespace donner::svg::components
