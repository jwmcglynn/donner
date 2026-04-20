#pragma once
/// @file

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor {

/// Collect every renderable \ref svg::SVGGeometryElement in @p root's
/// subtree (including @p root itself if it is geometry). Skips container
/// subtrees that are not part of the visual tree — \c defs, \c clipPath,
/// \c mask, \c filter, \c pattern, gradients, \c symbol, \c marker, \c
/// style.
///
/// Used by the editor to expand a group selection ("I picked this
/// `<g filter>`") into the set of leaves whose outlines + world bounds
/// should drive selection chrome.
[[nodiscard]] std::vector<svg::SVGGeometryElement> CollectRenderableGeometry(
    const svg::SVGElement& root);

/// Snapshot world-space bounds for every selected element that has
/// renderable geometry. Elements that are themselves geometry contribute
/// their own bounds; group-like elements contribute the union of their
/// renderable geometry descendants. Elements with no renderable geometry
/// produce no entry.
///
/// @param selection Current selection handles.
/// @return Document-space AABBs in selection order (one per selected
///   element that has geometry).
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
