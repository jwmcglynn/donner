#include "donner/svg/renderer/LayerDecomposition.h"

#include <algorithm>

#include "donner/svg/components/LayerMembershipComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg {

namespace {

/// An entity range in the flat render tree, identified by draw order.
struct EntityRange {
  int firstDrawOrder = 0;
  int lastDrawOrder = 0;
  Entity firstEntity = entt::null;
  Entity lastEntity = entt::null;
};

/// Build a list of all entities and a map from entity to draw order.
struct FlatEntityList {
  struct Entry {
    Entity entity;
    int drawOrder;
    bool isolatedLayer;
    bool hasSubtreeInfo;
    int subtreeFirstDrawOrder;
    int subtreeLastDrawOrder;
    Entity subtreeFirstEntity;
    Entity subtreeLastEntity;
  };

  std::vector<Entry> entries;

  void build(Registry& registry) {
    RenderingInstanceView view(registry);
    while (!view.done()) {
      const auto& instance = view.get();
      const Entity entity = view.currentEntity();

      Entry entry;
      entry.entity = entity;
      entry.drawOrder = instance.drawOrder;
      entry.isolatedLayer = instance.isolatedLayer;
      entry.hasSubtreeInfo = instance.subtreeInfo.has_value();

      if (instance.subtreeInfo) {
        // Look up the draw orders for the subtree boundary entities.
        const auto& first =
            registry.get<components::RenderingInstanceComponent>(instance.subtreeInfo->firstRenderedEntity);
        const auto& last =
            registry.get<components::RenderingInstanceComponent>(instance.subtreeInfo->lastRenderedEntity);
        entry.subtreeFirstDrawOrder = first.drawOrder;
        entry.subtreeLastDrawOrder = last.drawOrder;
        entry.subtreeFirstEntity = instance.subtreeInfo->firstRenderedEntity;
        entry.subtreeLastEntity = instance.subtreeInfo->lastRenderedEntity;
      } else {
        entry.subtreeFirstDrawOrder = instance.drawOrder;
        entry.subtreeLastDrawOrder = instance.drawOrder;
        entry.subtreeFirstEntity = entity;
        entry.subtreeLastEntity = entity;
      }

      entries.push_back(entry);
      view.advance();
    }
  }

  /// Find the index of an entity by its entt::entity value.
  int findIndex(Entity entity) const {
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
      if (entries[i].entity == entity) {
        return i;
      }
    }
    return -1;
  }
};

/// Expand promoted entities to their subtree ranges, then escalate through
/// compositing context ancestors.
std::vector<EntityRange> computePromotedRanges(const FlatEntityList& entityList,
                                               const std::vector<Entity>& promotedEntities) {
  if (promotedEntities.empty() || entityList.entries.empty()) {
    return {};
  }

  std::vector<EntityRange> ranges;

  for (const Entity promoted : promotedEntities) {
    const int idx = entityList.findIndex(promoted);
    if (idx < 0) {
      continue;
    }

    const auto& entry = entityList.entries[idx];

    // Start with the entity's own subtree range.
    EntityRange range;
    range.firstDrawOrder = entry.subtreeFirstDrawOrder;
    range.lastDrawOrder = entry.subtreeLastDrawOrder;
    range.firstEntity = entry.subtreeFirstEntity;
    range.lastEntity = entry.subtreeLastEntity;

    // If this entity has no subtree, it's a single entity.
    if (!entry.hasSubtreeInfo) {
      range.firstDrawOrder = entry.drawOrder;
      range.lastDrawOrder = entry.drawOrder;
      range.firstEntity = entry.entity;
      range.lastEntity = entry.entity;
    }

    // Escalate: walk backwards through the entity list to find ancestors with
    // compositing contexts (isolatedLayer = true).  An entity A is an ancestor
    // if A has subtreeInfo and A's subtree range contains our range.
    for (int i = idx - 1; i >= 0; --i) {
      const auto& ancestor = entityList.entries[i];
      if (!ancestor.hasSubtreeInfo) {
        continue;
      }
      // Check if ancestor's subtree contains our current range.
      if (ancestor.subtreeFirstDrawOrder <= range.firstDrawOrder &&
          ancestor.subtreeLastDrawOrder >= range.lastDrawOrder) {
        // This ancestor contains us. Escalate if it's a compositing context.
        if (ancestor.isolatedLayer) {
          range.firstDrawOrder = ancestor.drawOrder;
          range.lastDrawOrder = ancestor.subtreeLastDrawOrder;
          range.firstEntity = ancestor.entity;
          range.lastEntity = ancestor.subtreeLastEntity;
        }
      }
    }

    ranges.push_back(range);
  }

  // Sort by firstDrawOrder and merge overlapping ranges.
  std::sort(ranges.begin(), ranges.end(),
            [](const EntityRange& a, const EntityRange& b) {
              return a.firstDrawOrder < b.firstDrawOrder;
            });

  std::vector<EntityRange> merged;
  for (const auto& r : ranges) {
    if (!merged.empty() && r.firstDrawOrder <= merged.back().lastDrawOrder) {
      // Overlapping or adjacent — extend.
      if (r.lastDrawOrder > merged.back().lastDrawOrder) {
        merged.back().lastDrawOrder = r.lastDrawOrder;
        merged.back().lastEntity = r.lastEntity;
      }
    } else {
      merged.push_back(r);
    }
  }

  return merged;
}

}  // namespace

LayerDecompositionResult decomposeIntoLayers(Registry& registry,
                                             const std::vector<Entity>& promotedEntities,
                                             CompositingLayer::Reason reason) {
  LayerDecompositionResult result;

  // Build flat entity list.
  FlatEntityList entityList;
  entityList.build(registry);

  if (entityList.entries.empty()) {
    return result;
  }

  // Compute promoted ranges with escalation.
  const std::vector<EntityRange> promotedRanges =
      computePromotedRanges(entityList, promotedEntities);

  // Walk the entity list and slice into layers.
  uint32_t nextLayerId = 0;
  size_t promotedIdx = 0;
  int i = 0;
  const int n = static_cast<int>(entityList.entries.size());

  while (i < n) {
    const auto& entry = entityList.entries[i];

    // Check if this entity starts a promoted range.
    if (promotedIdx < promotedRanges.size() &&
        entry.drawOrder >= promotedRanges[promotedIdx].firstDrawOrder) {
      const auto& range = promotedRanges[promotedIdx];

      // Create dynamic layer for this promoted range.
      CompositingLayer layer;
      layer.id = nextLayerId++;
      layer.firstEntity = range.firstEntity;
      layer.lastEntity = range.lastEntity;
      layer.reason = reason;
      result.layers.push_back(layer);

      // Skip past all entities in the promoted range.
      while (i < n && entityList.entries[i].drawOrder <= range.lastDrawOrder) {
        ++i;
      }
      ++promotedIdx;
    } else {
      // Static region. Accumulate entities until we hit a promoted range or end.
      Entity staticFirst = entry.entity;
      Entity staticLast = entry.entity;

      while (i < n) {
        // Check if the next entity would enter a promoted range.
        if (promotedIdx < promotedRanges.size() &&
            entityList.entries[i].drawOrder >= promotedRanges[promotedIdx].firstDrawOrder) {
          break;
        }
        staticLast = entityList.entries[i].entity;
        ++i;
      }

      CompositingLayer layer;
      layer.id = nextLayerId++;
      layer.firstEntity = staticFirst;
      layer.lastEntity = staticLast;
      layer.reason = CompositingLayer::Reason::Static;
      result.layers.push_back(layer);
    }
  }

  // Assign LayerMembershipComponent to all entities.
  // Walk the entity list and layers together.
  size_t entityIdx = 0;
  for (const auto& layer : result.layers) {
    while (entityIdx < entityList.entries.size()) {
      const auto& entry = entityList.entries[entityIdx];
      registry.emplace_or_replace<components::LayerMembershipComponent>(entry.entity,
                                                                        layer.id);
      const bool isLast = (entry.entity == layer.lastEntity);
      ++entityIdx;
      if (isLast) {
        break;
      }
    }
  }

  return result;
}

}  // namespace donner::svg
