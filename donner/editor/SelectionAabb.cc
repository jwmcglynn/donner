#include "donner/editor/SelectionAabb.h"

#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor {

std::vector<Box2d> SnapshotSelectionWorldBounds(std::span<const svg::SVGElement> selection) {
  std::vector<Box2d> bounds;
  bounds.reserve(selection.size());
  for (const auto& element : selection) {
    if (!element.isa<svg::SVGGeometryElement>()) {
      continue;
    }

    const auto geometry = element.cast<svg::SVGGeometryElement>();
    if (auto worldBounds = geometry.worldBounds(); worldBounds.has_value()) {
      bounds.push_back(*worldBounds);
    }
  }

  return bounds;
}

void PromoteSelectionBoundsIfReady(SelectionBoundsCache& cache, std::uint64_t displayedDocVersion) {
  if (cache.pendingVersion != displayedDocVersion) {
    return;
  }

  cache.displayedBoundsDoc = cache.pendingBoundsDoc;
  cache.pendingBoundsDoc.clear();
  cache.pendingVersion = 0;
}

void RefreshSelectionBoundsCache(SelectionBoundsCache& cache,
                                 std::span<const svg::SVGElement> selection,
                                 std::uint64_t currentDocVersion,
                                 std::uint64_t displayedDocVersion) {
  cache.lastSelection.assign(selection.begin(), selection.end());
  cache.lastRefreshVersion = currentDocVersion;
  cache.pendingBoundsDoc = SnapshotSelectionWorldBounds(selection);
  cache.pendingVersion = currentDocVersion;

  if (selection.empty()) {
    cache.displayedBoundsDoc.clear();
  }

  PromoteSelectionBoundsIfReady(cache, displayedDocVersion);
}

}  // namespace donner::editor
