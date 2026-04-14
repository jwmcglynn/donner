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

/// Map cached document-space selection bounds into on-screen rectangles.
///
/// The return order is:
///   1. One screen-space rect per cached element bound.
///   2. One combined screen-space rect appended at the end when more than
///      one element contributes bounds.
///
/// @param viewport Current render-pane viewport snapshot.
/// @param selectionBoundsDoc Cached document-space bounds.
/// @return Screen-space rectangles for immediate-mode drawing.
[[nodiscard]] std::vector<Box2d> ComputeSelectionAabbScreenRects(
    const ViewportState& viewport, std::span<const Box2d> selectionBoundsDoc);

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

}  // namespace donner::editor
