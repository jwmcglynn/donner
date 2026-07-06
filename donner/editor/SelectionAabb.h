#pragma once
/// @file

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
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
/// \ref CollectRenderableGeometry and does not descend into text content -
/// tspans contribute chrome through their text root.
[[nodiscard]] std::vector<svg::SVGTextElement> CollectRenderableTextRoots(
    const svg::SVGElement& root);

/// Document-space AABB of @p text's laid-out glyph ink, or nullopt when the
/// text has no ink (empty content or unlaid-out text). The text-local ink
/// box is mapped corner-by-corner through the element's transform, so a
/// rotated text still produces a covering axis-aligned box.
[[nodiscard]] std::optional<Box2d> TextWorldInkBounds(const svg::SVGTextElement& text);

/// Text-local frame rect authored by the text tool: the
/// `data-donner-text-box-width`/`-height` region anchored one font-size
/// above the `x`/`y` origin (inverting the tool's "first baseline sits one
/// font-size below the box top" rule). Nullopt for point text.
[[nodiscard]] std::optional<Box2d> AuthoredTextBoxLocal(const svg::SVGTextElement& text);

/// Document-space frame of @p text: the authored text box when present (the
/// region the user dragged out), otherwise the laid-out ink bounds (the
/// computed extent of a point-text span). This is the rect that selection
/// chrome and transform handles anchor to.
[[nodiscard]] std::optional<Box2d> TextWorldFrameBounds(const svg::SVGTextElement& text);

/// Text-local frame rect of @p text: the authored text box when present,
/// otherwise the laid-out ink bounds - the same choice as
/// \ref TextWorldFrameBounds but in the text's LOCAL (pre-transform) space,
/// where it is axis-aligned. Nullopt when the text has no frame.
[[nodiscard]] std::optional<Box2d> TextFrameLocal(const svg::SVGTextElement& text);

/// ORIENTED document-space corners (local TL, TR, BR, BL order) of @p text's
/// frame: the local frame mapped corner-by-corner through the text's
/// transform. Unlike \ref TextWorldFrameBounds this carries the rotation
/// instead of collapsing to the axis-aligned envelope. Nullopt when the text
/// has no frame.
[[nodiscard]] std::optional<std::array<Vector2d, 4>> TextWorldFrameCorners(
    const svg::SVGTextElement& text);

/// Local frame + local-to-document transform for a `<text>` element, the
/// inputs an oriented selection frame and its transform handles are built
/// from.
struct TextFramePlacement {
  /// Axis-aligned frame in the text's local (pre-transform) space.
  Box2d frameLocal;
  /// Local-to-document transform for the text element.
  Transform2d documentFromText;
};

/// Placement for the select tool's ORIENTED `<text>` frame: returned only when
/// @p selection is a single renderable `<text>` (or an element resolving to
/// one) whose frame is actually oriented - its transform rotates or skews the
/// frame off the axis-aligned envelope. Nullopt for multi-selection, non-text
/// selections, text with no frame, or an axis-aligned text frame; in those
/// cases callers keep the existing axis-aligned selection chrome. The oriented
/// corners are \ref FrameCornersDoc of the returned placement.
[[nodiscard]] std::optional<TextFramePlacement> SingleOrientedTextSelectionPlacement(
    std::span<const svg::SVGElement> selection);

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
