#pragma once
/// @file
///
/// `LayersPanel` is the user-facing Layers panel UI (design docs 0046 "Editor
/// Group Layers" and 0047 "v0.8 Showcase" Milestone 3). It renders the flat
/// `LayerTreeModel` row list as an ImGui tree with disclosure chevrons, per-row
/// preview swatches, and selection that stays synchronized with the canvas and
/// source panes.
///
/// Selection requests flow out through the live `EditorApp` (`setSelection`,
/// `toggleInSelection`), mirroring `SidebarPresenter`'s discipline: when the
/// async renderer owns the document, clicks are dropped rather than racing the
/// worker.
///
/// The panel deliberately exposes non-ImGui testing seams (`rows`,
/// `handleRowClick`, `hasThumbnailOrSwatch`, `rowFallbackSwatch`) so the
/// selection logic and preview guarantees can be exercised without an ImGui
/// frame.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/LayerTreeModel.h"

namespace donner::editor {

/// ImGui Layers panel backed by a `LayerTreeModel` snapshot.
class LayersPanel {
public:
  LayersPanel() = default;

  /// Modifier state for a row click. Mirrors the ImGui click path so tests can
  /// drive selection without a live ImGui frame.
  struct ClickModifiers {
    bool shift = false;  ///< Shift extends selection in visible row order.
    bool ctrl = false;   ///< Ctrl/Cmd toggles the clicked row in the selection.
  };

  /// Rebuild the row snapshot and per-row preview swatches from live editor
  /// state. Safe to call only when the async renderer is idle (delegates to
  /// `LayerTreeModel::refresh`).
  void refreshSnapshot(const EditorApp& app);

  /// Render the panel into the current ImGui window. Must be called inside an
  /// `ImGui::Begin(...) / End()` pair. When @p liveApp is null (the worker owns
  /// the document) mutating clicks are dropped.
  void render(EditorApp* liveApp);

  /// The current flat row list (testing/diagnostics accessor).
  [[nodiscard]] const std::vector<LayerTreeRow>& rows() const { return model_.rows(); }

  /// Number of currently-visible rows (rows in the flat list).
  [[nodiscard]] std::size_t visibleRowCount() const { return model_.rows().size(); }

  /// Apply the selection logic for a click on the visible row at @p rowIndex.
  /// This is the exact logic the ImGui click path uses, factored out so it is
  /// unit-testable without an ImGui context.
  ///
  /// @param app Live editor app the selection request is applied to.
  /// @param rowIndex Index into `rows()`.
  /// @param mods Modifier-key state for the click.
  void handleRowClick(EditorApp& app, std::size_t rowIndex, ClickModifiers mods);

  /// Whether the row identified by @p stableId has a non-null thumbnail handle
  /// or a deterministic fallback swatch. Always true for every visible row.
  [[nodiscard]] bool hasThumbnailOrSwatch(std::uint64_t stableId) const;

  /// The fallback preview swatch color for the row identified by @p stableId,
  /// or `std::nullopt` if the row is not present.
  [[nodiscard]] std::optional<css::RGBA> rowFallbackSwatch(std::uint64_t stableId) const;

  /// Whether the most recent `render` / `handleRowClick` changed the editor
  /// selection. Consumes the flag. Used by `EditorShell` to fire the existing
  /// canvas/source selection-sync plumbing when a Layers row drives selection.
  [[nodiscard]] bool consumeSelectionChanged();

  /// Access the underlying model (testing/diagnostics).
  [[nodiscard]] const LayerTreeModel& model() const { return model_; }

  /// A real per-element geometry thumbnail: the element's computed outline
  /// sampled into a polyline and normalized into the unit square `[0,1]^2`,
  /// alongside its computed fill/stroke. Drawn into the row's 24×24 preview cell.
  struct RowThumbnail {
    /// Outline points in normalized `[0,1]` coordinates (y-down, matching ImGui).
    std::vector<Vector2d> normalizedPoints;
    /// Whether the outline encloses an area (drawn filled) vs. an open stroke.
    bool closed = false;
    /// Computed fill color (used when `closed`); falls back to transparent.
    css::RGBA fill = css::RGBA(0, 0, 0, 0);
    /// Computed stroke color for the outline.
    css::RGBA stroke = css::RGBA(0, 0, 0, 255);
  };

  /// The geometry thumbnail for the row identified by @p stableId, or
  /// `std::nullopt` when the row has no real geometry preview (it then uses the
  /// fallback swatch). Testing/diagnostics accessor.
  [[nodiscard]] std::optional<RowThumbnail> rowThumbnail(std::uint64_t stableId) const;

private:
  /// Find the model index of the row with @p stableId, or std::nullopt.
  [[nodiscard]] std::optional<std::size_t> rowIndexForStableId(std::uint64_t stableId) const;

  LayerTreeModel model_;
  /// Per-row fallback swatch colors keyed by stable id. Rebuilt on every
  /// refresh so it tracks the current document and styles.
  std::unordered_map<std::uint64_t, css::RGBA> swatchByStableId_;
  /// Per-row geometry thumbnails keyed by stable id. For geometry rows
  /// (path/rect/circle/…) this holds the element's computed outline normalized
  /// into the unit square plus its computed fill/stroke, so the row icon is a
  /// real per-shape silhouette rather than a flat color block. Non-geometry rows
  /// (groups, text, image, use) have no entry and fall back to the swatch.
  ///
  /// A full offscreen subtree *raster* (rendering each layer's pixels through the
  /// SVG renderer) is deferred: `RendererTinySkia` only renders a whole
  /// `SVGDocument`, and there is no subtree-render entrypoint reachable from this
  /// panel, so a faithful raster would require a cross-module renderer change.
  /// The vector silhouette below is the real per-element preview achievable
  /// in-panel (design doc 0046 Milestone 3 follow-up).
  std::unordered_map<std::uint64_t, RowThumbnail> thumbnailByStableId_;
  /// Visible-row index of the most recent plain/ctrl click; the anchor for
  /// shift-range selection.
  std::optional<std::size_t> anchorRowIndex_;
  /// Active keyboard-navigation row index within the visible list.
  std::optional<std::size_t> activeRowIndex_;
  /// Set when render/handleRowClick changed selection; cleared by
  /// `consumeSelectionChanged`.
  bool selectionChanged_ = false;
};

}  // namespace donner::editor
