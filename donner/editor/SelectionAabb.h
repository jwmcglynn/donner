#pragma once
/// @file

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGTextElement.h"

namespace donner::editor {

/// Collect every renderable \ref svg::SVGGeometryElement in @p root's
/// subtree (including @p root itself if it is geometry). Skips container
/// subtrees that are not part of the visual tree - \c defs, \c clipPath,
/// \c mask, \c filter, \c pattern, gradients, \c symbol, \c marker, \c
/// style.
///
/// Used by the editor to expand a group selection ("I picked this
/// `<g filter>`") into the set of leaves whose outlines + world bounds
/// should drive selection chrome.
[[nodiscard]] std::vector<svg::SVGGeometryElement> CollectRenderableGeometry(
    const svg::SVGElement& root);

/// Collect every renderable `<text>` root in @p root's subtree (including
/// @p root itself if it is one). Skips the same non-rendered containers as
/// \ref CollectRenderableGeometry and does not descend into text content —
/// tspans contribute chrome through their text root.
[[nodiscard]] std::vector<svg::SVGTextElement> CollectRenderableTextRoots(
    const svg::SVGElement& root);

/// Document-space AABB of @p text's laid-out glyph ink, or nullopt when the
/// text has no ink (empty content or unlaid-out text). The text-local ink
/// box is mapped corner-by-corner through the element's transform, so a
/// rotated text still produces a covering axis-aligned box.
[[nodiscard]] std::optional<Box2d> TextWorldInkBounds(const svg::SVGTextElement& text);

/// Snapshot world-space bounds for every selected element that has
/// renderable content. Elements that are themselves geometry contribute
/// their own bounds; `<text>` roots contribute their laid-out ink bounds;
/// group-like elements contribute the union of their renderable
/// descendants. Elements with no renderable content produce no entry.
///
/// @param selection Current selection handles.
/// @return Document-space AABBs in selection order (one per selected
///   element that has geometry).
[[nodiscard]] std::vector<Box2d> SnapshotSelectionWorldBounds(
    std::span<const svg::SVGElement> selection);

/// Snapshot world-space bounds for renderable content (geometry and text)
/// painted after a single selected element. This is a conservative occlusion hint for the
/// async-safe re-drag path: if the click falls inside one of these bounds,
/// the editor should wait for the normal hit-test path instead of assuming
/// the selected element owns the click.
///
/// Multi-selection produces no occlusion hints because the fast re-drag
/// path is single-selection only.
///
/// @param selection Current selection handles.
/// @return Document-space AABBs for later-painted renderable geometry.
[[nodiscard]] std::vector<Box2d> SnapshotSelectionOccludingWorldBounds(
    std::span<const svg::SVGElement> selection);

/// Pending/displayed selection AABBs tracked across document-version changes.
struct SelectionBoundsCache {
  std::vector<svg::SVGElement> lastSelection;
  std::vector<Box2d> pendingBoundsDoc;
  std::vector<Box2d> pendingOccludingBoundsDoc;
  std::uint64_t pendingVersion = 0;
  std::vector<Box2d> displayedBoundsDoc;
  std::vector<Box2d> displayedOccludingBoundsDoc;
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
