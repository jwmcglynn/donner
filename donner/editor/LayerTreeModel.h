#pragma once
/// @file
///
/// `LayerTreeModel` is the pure, ImGui-free, value-snapshot model behind the
/// user-facing Layers panel.
///
/// It walks the editable SVG light tree and produces a flat list of
/// `LayerTreeRow`s — one per editor-visible layer (document root, groups,
/// subgroups, and renderable leaf shapes). Non-rendered resource subtrees
/// (`<defs>`, gradients, filters, clip paths, masks, patterns, markers,
/// symbols, `<style>`, `<title>`, `<desc>`, `<metadata>`) are excluded.
///
/// The model deliberately knows nothing about ImGui or rendering backends; it
/// is a DOM-shaped snapshot that `LayersPanel` renders and that tests can
/// assert against directly. Expansion state persists across refreshes keyed by
/// a stable per-element id so collapsing/expanding survives idle snapshot
/// refreshes and document mutations that don't rebuild the tree.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Classification of an editor layer row. Drives the row icon/affordances and
/// is asserted by `layer_tree_model_tests`.
enum class LayerRowKind : std::uint8_t {
  Root,          ///< The document root `<svg>`.
  Group,         ///< A grouping container (`<g>` or nested `<svg>`) with renderable descendants.
  CompoundPath,  ///< A `<path>` with more than one subpath (kept as a single leaf row).
  Shape,         ///< A renderable leaf shape (path/rect/circle/ellipse/line/poly*/text/image/use).
  Other,         ///< Catch-all for renderable elements that don't fit the buckets above.
};

/// One flattened row of the Layers tree. The list is flat (not nested); nesting
/// is conveyed by `depth`. Rows for descendants of a collapsed group are
/// omitted, but `hasChildren` is still set on collapsed groups so the
/// disclosure chevron renders.
struct LayerTreeRow {
  /// Indentation depth. The root row is depth 0.
  int depth = 0;
  /// Display name: the element `id` when non-empty, otherwise `<tag>[n]` where
  /// `n` is the element's zero-based index among its same-tag siblings.
  std::string displayName;
  /// Stable identity used as the expansion/selection key. Derived from the
  /// element's ECS entity id; stable across refreshes until a full document
  /// rebuild (the same lifetime guarantee `SidebarPresenter` relies on).
  std::uint64_t stableId = 0;
  /// The editable SVG element this row maps to. Valid until a full document
  /// rebuild, at which point the model is refreshed on the next idle frame.
  svg::SVGElement element;
  /// True when this row has editor-visible children (so a chevron is shown).
  bool hasChildren = false;
  /// True when this row is currently expanded (only meaningful when
  /// `hasChildren`).
  bool isExpanded = false;
  /// True when the element is visible (display != none, visibility != hidden).
  bool isVisible = true;
  /// True when the element (or one of its ancestors) is locked via the
  /// `data-donner-locked="true"` marker attribute. Locked rows are protected
  /// from geometry-changing edits and deletion (see `IsLocked` in
  /// `LockState.h`).
  bool isLocked = false;
  /// True when the element is in the editor selection.
  bool isSelected = false;
  /// True for a group that is not itself selected but has at least one
  /// transitive descendant in the selection (some-but-not-all).
  bool isPartiallySelected = false;
  /// Row classification.
  LayerRowKind kind = LayerRowKind::Other;
};

/// Builds and owns the flat Layers row list. Selection is mirrored in from
/// `EditorApp` on every `refresh`; selection *requests* flow back out through
/// `LayersPanel` (this model is read-only with respect to the document).
class LayerTreeModel {
public:
  LayerTreeModel() = default;

  /// Rebuild the row snapshot from live editor state. Safe to call only when
  /// the async renderer is idle (mirrors `SidebarPresenter::refreshSnapshot`,
  /// taking document write access for the duration of the walk). Preserves
  /// expansion state across calls.
  void refresh(const EditorApp& app);

  /// The current flat row list, in visual stack order (later-painted siblings
  /// appear first / on top).
  [[nodiscard]] const std::vector<LayerTreeRow>& rows() const { return rows_; }

  /// Set the expansion state for the element identified by `stableId`.
  void setExpanded(std::uint64_t stableId, bool expanded);

  /// Toggle the expansion state for the element identified by `stableId`.
  void toggleExpanded(std::uint64_t stableId);

  /// Whether the element identified by `stableId` is currently expanded.
  [[nodiscard]] bool isExpanded(std::uint64_t stableId) const;

  /// Compute the stable id for an element (public so panels/tests can key into
  /// expansion state without duplicating the derivation).
  [[nodiscard]] static std::uint64_t StableIdFor(const svg::SVGElement& element);

private:
  /// Recursively append rows for `element` and (when expanded) its editor-
  /// visible descendants.
  void appendRows(const svg::SVGElement& element, int depth,
                  const std::vector<svg::SVGElement>& selection);

  std::vector<LayerTreeRow> rows_;
  /// Persistent expansion state keyed by stable id. Survives `refresh`.
  std::unordered_set<std::uint64_t> expanded_;
  /// True once the first refresh has run (so the root can be default-expanded
  /// exactly once without re-expanding after the user collapses it).
  bool initialized_ = false;
};

}  // namespace donner::editor
