#include "donner/editor/SelectionAabb.h"

#include <optional>

#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor {

namespace {

[[nodiscard]] std::optional<Box2d> CombinedBounds(std::span<const Box2d> bounds) {
  if (bounds.empty()) {
    return std::nullopt;
  }

  Box2d combined = bounds.front();
  for (std::size_t i = 1; i < bounds.size(); ++i) {
    combined.addBox(bounds[i]);
  }
  return combined;
}

}  // namespace

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

std::vector<Box2d> ComputeSelectionAabbScreenRects(const ViewportState& viewport,
                                                   std::span<const Box2d> selectionBoundsDoc) {
  std::vector<Box2d> rects;
  rects.reserve(selectionBoundsDoc.size() + (selectionBoundsDoc.size() > 1 ? 1u : 0u));
  for (const Box2d& boundsDoc : selectionBoundsDoc) {
    rects.push_back(viewport.documentToScreen(boundsDoc));
  }

  if (selectionBoundsDoc.size() > 1) {
    if (auto combined = CombinedBounds(selectionBoundsDoc); combined.has_value()) {
      rects.push_back(viewport.documentToScreen(*combined));
    }
  }

  return rects;
}

}  // namespace donner::editor
