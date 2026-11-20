#pragma once
/// @file
/// Finds the rendering instances that exist only as content drawn for another instance.

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg::compositor {

/**
 * Closed draw-order ranges of the mask, marker and pattern content drawn for rendering instances,
 * sorted and merged.
 *
 * That content is instantiated as rendering instances of its own, but it reaches the frame only
 * through the element that draws it. It is never a candidate for a layer of its own: such a layer
 * would be composed onto the frame as document content.
 *
 * @param registry Registry holding the rendering instances.
 */
[[nodiscard]] inline std::vector<std::pair<int, int>> DrawnContentDrawOrders(Registry& registry) {
  std::vector<std::pair<int, int>> ranges;
  auto view = registry.view<components::RenderingInstanceComponent>();
  for (auto entity : view) {
    view.get<components::RenderingInstanceComponent>(entity).forEachDrawnContent(
        [&](const components::SubtreeInfo& content) {
          const auto* first =
              registry.try_get<components::RenderingInstanceComponent>(content.firstRenderedEntity);
          const auto* last =
              registry.try_get<components::RenderingInstanceComponent>(content.lastRenderedEntity);
          if (first != nullptr && last != nullptr) {
            ranges.emplace_back(first->drawOrder, std::max(first->drawOrder, last->drawOrder));
          }
        });
  }
  std::sort(ranges.begin(), ranges.end());
  std::vector<std::pair<int, int>> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.first <= merged.back().second) {
      merged.back().second = std::max(merged.back().second, range.second);
    } else {
      merged.push_back(range);
    }
  }
  return merged;
}

/**
 * Whether a draw order lies in one of the ranges returned by \ref DrawnContentDrawOrders.
 *
 * @param ranges Sorted, merged closed ranges.
 * @param drawOrder Draw order to look up.
 */
[[nodiscard]] inline bool InDrawOrderRanges(const std::vector<std::pair<int, int>>& ranges,
                                            int drawOrder) {
  const auto it = std::upper_bound(
      ranges.begin(), ranges.end(), drawOrder,
      [](int order, const std::pair<int, int>& range) { return order < range.first; });
  return it != ranges.begin() && drawOrder <= std::prev(it)->second;
}

}  // namespace donner::svg::compositor
