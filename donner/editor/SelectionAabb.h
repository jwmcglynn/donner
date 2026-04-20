#pragma once
/// @file

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Snapshot world-space bounds for every selected geometry element that
/// currently has computable bounds.
///
/// @param selection Current selection handles.
/// @return Document-space AABBs in selection order.
[[nodiscard]] std::vector<Box2d> SnapshotSelectionWorldBounds(
    std::span<const svg::SVGElement> selection);

/// Pending/displayed selection AABBs tracked across document-version changes.
struct SelectionBoundsCache {
  std::vector<svg::SVGElement> lastSelection;
  std::vector<Box2d> pendingBoundsDoc;
  std::uint64_t pendingVersion = 0;
  std::vector<Box2d> displayedBoundsDoc;
  std::uint64_t lastRefreshVersion = std::numeric_limits<std::uint64_t>::max();
};

/// Promote pending bounds when the corresponding document bitmap is visible.
void PromoteSelectionBoundsIfReady(SelectionBoundsCache& cache, std::uint64_t displayedDocVersion);

/// Refresh the cache from the current selection and document version.
void RefreshSelectionBoundsCache(SelectionBoundsCache& cache,
                                 std::span<const svg::SVGElement> selection,
                                 std::uint64_t currentDocVersion,
                                 std::uint64_t displayedDocVersion);

/// Compute screen-space rectangles for each document-space AABB.  When more
/// than one AABB is provided, an additional bounding rect is appended.
///
/// @param viewport Current viewport state.
/// @param selectionBoundsDoc Document-space AABBs.
/// @return Screen-space rects (per-element + optional combined).
[[nodiscard]] std::vector<Box2d> ComputeSelectionAabbScreenRects(
    const ViewportState& viewport, std::span<const Box2d> selectionBoundsDoc);

}  // namespace donner::editor
