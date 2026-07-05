#pragma once
/// @file
///
/// `LayersPanel` is the user-facing Layers panel UI. It renders the flat
/// `LayerTreeModel` row list as an ImGui tree with disclosure chevrons, per-row
/// preview thumbnails, and selection that stays synchronized with the canvas and
/// source panes.
///
/// Selection requests flow out through the live `EditorApp` (`setSelection`,
/// `toggleInSelection`), mirroring `SidebarPresenter`'s discipline: when the
/// async renderer owns the document, clicks are dropped rather than racing the
/// worker.
///
/// The panel deliberately exposes non-ImGui testing seams (`rows`,
/// `handleRowClick`, `hasThumbnailOrSwatch`, `rowFallbackSwatch`,
/// `rowThumbnail`) so the selection logic and preview guarantees can be
/// exercised without an ImGui frame.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerTreeModel.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {
class Renderer;
}

namespace donner::editor {

/// Current "this element is locked, you can't select it" flash fed into the
/// Layers panel so the rejected element's row flashes red in sync with the
/// canvas outline flash. Mirrors `OverlayRenderer.h`'s `LockedRejectionFlashInput`
/// but is kept local so `LayersPanel` stays decoupled from `SelectTool` /
/// `OverlayRenderer`; `EditorShell` maps the tool's flash into this struct.
struct LayersLockedRejectionFlash {
  /// The element whose selection was rejected because it (or an ancestor group) is locked.
  svg::SVGElement element;
  /// Fade intensity in (0, 1]; scales the red row-highlight alpha at draw time.
  float intensity = 0.0f;
};

/// ImGui Layers panel backed by a `LayerTreeModel` snapshot.
class LayersPanel {
public:
  LayersPanel() = default;

  /// Maps a per-row rendered thumbnail bitmap to an ImGui texture handle for
  /// display. The editor shell supplies an implementation backed by
  /// `GlTextureCache` (the same Donner-bitmap -> GL/WGPU texture path the render
  /// pane uses); when null (e.g. in headless unit tests) the panel falls back to
  /// the deterministic swatch. Donner renders the thumbnail pixels; ImGui only
  /// blits the resulting texture - see CLAUDE.md "No Rendering Vector Graphics
  /// With ImGui".
  ///
  /// @param stableId Stable id of the row whose thumbnail is being uploaded.
  /// @param bitmap The Donner-rendered RGBA thumbnail bitmap.
  /// @return An ImGui texture handle plus the valid payload UV range, or an
  ///   empty texture if upload failed.
  struct ThumbnailTexture {
    ImTextureID texture = 0;                      ///< ImGui texture handle.
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);  ///< Bottom-right valid payload UV.
  };
  using ThumbnailTextureProvider =
      std::function<ThumbnailTexture(std::uint64_t stableId, const svg::RendererBitmap& bitmap)>;

  /// Maps a static layer-affordance icon bitmap to an ImGui texture handle for
  /// display. The icon bitmaps are rendered from embedded Bootstrap SVG resources
  /// through Donner; ImGui only receives the final raster texture for the image
  /// button.
  ///
  /// @param stableId Stable id of the static icon resource being uploaded.
  /// @param bitmap The Donner-rendered RGBA icon bitmap.
  /// @return An ImGui texture handle plus the valid payload UV range, or an
  ///   empty texture if upload failed.
  struct IconTexture {
    ImTextureID texture = 0;                      ///< ImGui texture handle.
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);  ///< Bottom-right valid payload UV.
  };
  using IconTextureProvider =
      std::function<IconTexture(std::uint64_t stableId, const svg::RendererBitmap& bitmap)>;

  /// Diagnostics for the most recent thumbnail refresh.
  struct ThumbnailRefreshStats {
    /// Document frame version observed during the refresh.
    std::uint64_t documentFrameVersion = 0;
    /// Visible row count after refreshing the model.
    std::size_t rowCount = 0;
    /// Number of row thumbnails rasterized through Donner during this refresh.
    std::size_t renderedCount = 0;
    /// Number of cached row thumbnails reused during this refresh.
    std::size_t reusedCount = 0;
    /// Number of rows skipped because the canvas render tree had pending invalidation.
    std::size_t skippedForCanvasInvalidationCount = 0;
    /// Wall time spent in thumbnail rasterization work.
    double renderMs = 0.0;
  };

  /// Modifier state for a row click. Mirrors the ImGui click path so tests can
  /// drive selection without a live ImGui frame.
  struct ClickModifiers {
    bool shift = false;  ///< Shift extends selection in visible row order.
    bool ctrl = false;   ///< Ctrl/Cmd toggles the clicked row in the selection.
  };

  /// Rebuild the row snapshot, per-row preview swatches, and per-row rendered
  /// thumbnail bitmaps from live editor state. Safe to call only when the async
  /// renderer is idle (delegates to `LayerTreeModel::refresh` and renders each
  /// row's element subtree through `svg::Renderer::renderElementToBitmap`, which
  /// takes document write access).
  ///
  /// @param app Live editor app to snapshot.
  /// @param renderer Optional renderer to use for thumbnail rasterization.
  void refreshSnapshot(const EditorApp& app, svg::Renderer* renderer = nullptr);

  /// Render the panel into the current ImGui window. Must be called inside an
  /// `ImGui::Begin(...) / End()` pair. When @p liveApp is null (the worker owns
  /// the document) mutating clicks are dropped.
  ///
  /// @param liveApp Live editor app for selection/mutation, or null.
  /// @param textureProvider Uploads a row's rendered thumbnail bitmap to an
  ///   ImGui texture for display, or null to fall back to the swatch.
  /// @param iconTextureProvider Uploads the static Donner-rendered layer icon
  ///   bitmaps to ImGui textures for display, or null to keep a blank hit area
  ///   in headless tests.
  void render(EditorApp* liveApp, const ThumbnailTextureProvider& textureProvider = {},
              const IconTextureProvider& iconTextureProvider = {});

  /// Set the current locked-rejection flash (or clear it with `std::nullopt`).
  /// When set with a positive intensity and the flashed element matches a
  /// visible row, `render` paints that row's background red with alpha scaled by
  /// `intensity`, in sync with the canvas outline flash. `EditorShell` calls this
  /// each frame with `SelectTool::lockedRejectionFlash()` mapped into the input
  /// struct, before `render`.
  void setLockedRejectionFlash(std::optional<LayersLockedRejectionFlash> flash);

  /// The visible row index that the active locked-rejection flash maps to (an
  /// exact element match against a visible row), or `std::nullopt` when no flash
  /// is active, its intensity is non-positive, or its element is not a visible
  /// row. The non-ImGui seam the row-flash test asserts on.
  [[nodiscard]] std::optional<std::size_t> flashedRowIndex() const;

  /// The fade intensity of the active locked-rejection flash, or 0 when none is
  /// set. Testing/diagnostics accessor paired with `flashedRowIndex`.
  [[nodiscard]] float lockedRejectionFlashIntensity() const {
    return lockedRejectionFlash_.has_value() ? lockedRejectionFlash_->intensity : 0.0f;
  }

  /// The current flat row list (testing/diagnostics accessor).
  [[nodiscard]] const std::vector<LayerTreeRow>& rows() const { return model_.rows(); }

  /// Diagnostics for the most recent thumbnail refresh.
  [[nodiscard]] const ThumbnailRefreshStats& thumbnailRefreshStats() const {
    return thumbnailRefreshStats_;
  }

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

  /// Toggle the visibility of the row at @p rowIndex via the shared
  /// `EditorApp::setElementVisible` path (the same path the context-menu
  /// Hide/Show items use). Factored out so the eye-button affordance is
  /// unit-testable without an ImGui frame. No-op for an out-of-range index.
  ///
  /// @param app Live editor app the mutation is applied to.
  /// @param rowIndex Index into `rows()`.
  void handleEyeClick(EditorApp& app, std::size_t rowIndex);

  /// Toggle the lock state of the row at @p rowIndex via the shared
  /// `EditorApp::setElementLocked` path (the same path the context-menu
  /// Lock/Unlock items use). Factored out so the lock-button affordance is
  /// unit-testable without an ImGui frame. No-op for an out-of-range index.
  ///
  /// @param app Live editor app the mutation is applied to.
  /// @param rowIndex Index into `rows()`.
  void handleLockClick(EditorApp& app, std::size_t rowIndex);

  /// Rename the element at @p rowIndex to @p newId via the shared
  /// `EditorApp::renameSelectedElement` path - a DOM-level id change that also
  /// repoints `url(#…)` / `href` references and `<style>` selectors. The row's
  /// element is selected first (renaming the thing you double-clicked), then the
  /// engine runs. Factored out so the inline-edit affordance is unit-testable
  /// without an ImGui frame.
  ///
  /// @param app Live editor app the mutation is applied to.
  /// @param rowIndex Index into `rows()`.
  /// @param newId The requested new element id.
  /// @return True if the rename was applied; false on out-of-range index or when
  ///   the engine rejects it (locked, empty, unchanged, or duplicate id).
  bool handleRowRename(EditorApp& app, std::size_t rowIndex, std::string_view newId);

  /// Move the element at @p fromIndex so it sits at the @p toIndex row's
  /// position, among the same parent's children, via the shared
  /// `EditorApp::reorderElementBeforeSibling` DOM move. Cross-parent drops and
  /// locked elements are rejected. Factored out so drag-to-reorder is
  /// unit-testable without an ImGui frame.
  ///
  /// @param app Live editor app the mutation is applied to.
  /// @param fromIndex Index of the dragged row.
  /// @param toIndex Index of the drop-target row.
  /// @return True if a move was applied; false on out-of-range/no-op/rejected.
  bool handleRowReorder(EditorApp& app, std::size_t fromIndex, std::size_t toIndex);

  /// Reorder the element at @p rowIndex in paint order (z-order) via the shared
  /// `EditorApp::reorderSelectedElement` engine - the same path as the canvas
  /// Arrange menu and the Cmd+[ / Cmd+] shortcuts. The row's element is selected
  /// first, then the move runs. Factored out so the Layers context-menu Arrange
  /// items are unit-testable without an ImGui frame.
  ///
  /// @param app Live editor app the mutation is applied to.
  /// @param rowIndex Index into `rows()`.
  /// @param direction Which way to move the element in paint order.
  /// @return True if the element moved; false on out-of-range index or when the
  ///   engine rejects it (root element, locked, or already at the extreme).
  bool handleRowZOrder(EditorApp& app, std::size_t rowIndex, EditorApp::ZOrder direction);

  /// Begin an inline rename of the row identified by @p stableId (the render
  /// loop draws an edit field in place of the row label). No-op if the row is
  /// not present. Exposed so the context-menu "Rename" item and tests can start
  /// an edit; the double-click path calls it internally.
  void beginRename(std::uint64_t stableId);

  /// The stable id of the row currently being inline-renamed, or `std::nullopt`.
  [[nodiscard]] std::optional<std::uint64_t> renamingStableId() const { return renamingStableId_; }

  /// Whether the row identified by @p stableId has a non-null thumbnail handle
  /// or a deterministic fallback swatch. Always true for every visible row.
  [[nodiscard]] bool hasThumbnailOrSwatch(std::uint64_t stableId) const;

  /// The fallback preview swatch color for the row identified by @p stableId,
  /// or `std::nullopt` if the row is not present.
  [[nodiscard]] std::optional<css::RGBA> rowFallbackSwatch(std::uint64_t stableId) const;

  /// The Donner-rendered preview thumbnail bitmap for the row identified by
  /// @p stableId, or `nullptr` when the row has no real raster preview (it then
  /// uses the fallback swatch). This is the non-ImGui data seam: tests assert on
  /// the rendered pixels here without needing a GL context or an ImGui frame.
  ///
  /// @param stableId Stable id of the row.
  [[nodiscard]] const svg::RendererBitmap* rowThumbnail(std::uint64_t stableId) const;

  /// Whether the most recent `render` / `handleRowClick` changed the editor
  /// selection. Consumes the flag. Used by `EditorShell` to fire the existing
  /// canvas/source selection-sync plumbing when a Layers row drives selection.
  [[nodiscard]] bool consumeSelectionChanged();

  /// Whether the most recent `render` queued a DOM mutation (show/hide, lock,
  /// rename, reorder, z-order) that is not reflected by a selection change.
  /// Consumes the flag. `EditorShell` flushes the queued mutation and
  /// re-renders when this is set; without it a show/hide toggle would leave the
  /// canvas showing the pre-mutation frame (the "hidden layer ghost" report).
  [[nodiscard]] bool consumeQueuedMutation();

  /// Record which visible row (if any) is under the mouse cursor. Called from
  /// `render` with the hovered row index, and directly by tests. Drives the
  /// canvas hover-highlight chrome the same way the source pane does. Passing
  /// `std::nullopt` (or an out-of-range index) clears the hover.
  void noteRowHovered(std::optional<std::size_t> rowIndex);

  /// The element under the mouse cursor as of the most recent `render` /
  /// `noteRowHovered`, or `std::nullopt` when no row is hovered.
  [[nodiscard]] std::optional<svg::SVGElement> hoveredElement() const { return hoveredElement_; }

  /// Access the underlying model (testing/diagnostics).
  [[nodiscard]] const LayerTreeModel& model() const { return model_; }

private:
  struct CachedThumbnail {
    svg::RendererBitmap bitmap;
    std::uint64_t documentFrameVersion = 0;
    Vector2i maxSizePx = Vector2i::Zero();
  };

  /// Find the model index of the row with @p stableId, or std::nullopt.
  [[nodiscard]] std::optional<std::size_t> rowIndexForStableId(std::uint64_t stableId) const;

  LayerTreeModel model_;
  /// Per-row fallback swatch colors keyed by stable id. Rebuilt on every
  /// refresh so it tracks the current document and styles.
  std::unordered_map<std::uint64_t, css::RGBA> swatchByStableId_;
  /// Per-row Donner-rendered preview thumbnails keyed by stable id. Each entry
  /// is the element's subtree rasterized through
  /// `svg::Renderer::renderElementToBitmap` into the preview cell size - a real
  /// render of the user's artwork, not an ImGui-synthesized silhouette. Rows
  /// whose subtree has no boundable geometry (empty groups, text, image, use)
  /// have no entry and fall back to the swatch.
  std::unordered_map<std::uint64_t, CachedThumbnail> thumbnailBitmapByStableId_;
  /// Diagnostics for the most recent thumbnail refresh.
  ThumbnailRefreshStats thumbnailRefreshStats_;
  /// Visible-row index of the most recent plain/ctrl click; the anchor for
  /// shift-range selection.
  std::optional<std::size_t> anchorRowIndex_;
  /// Active keyboard-navigation row index within the visible list.
  std::optional<std::size_t> activeRowIndex_;
  /// Set when render/handleRowClick changed selection; cleared by
  /// `consumeSelectionChanged`.
  bool selectionChanged_ = false;
  /// Set when a handler queued a DOM mutation (show/hide, lock, rename, reorder,
  /// z-order); cleared by `consumeQueuedMutation`.
  bool mutationQueued_ = false;
  /// Element under the mouse cursor as of the most recent render, or nullopt.
  std::optional<svg::SVGElement> hoveredElement_;
  /// Active locked-rejection flash (the rejected element + fade intensity), or
  /// nullopt when no locked click is being rejected. `render` paints a red
  /// background behind the matching row with alpha scaled by `intensity`.
  std::optional<LayersLockedRejectionFlash> lockedRejectionFlash_;
  /// Stable id of the row currently being inline-renamed, or nullopt when no
  /// edit is in progress. The render loop draws an `InputText` for this row.
  std::optional<std::uint64_t> renamingStableId_;
  /// Edit buffer backing the inline rename `InputText`. Seeded from the row's
  /// current display name when the edit begins.
  std::string renameBuffer_;
  /// One-shot: request keyboard focus on the rename `InputText` the next frame
  /// it is drawn (set when an edit begins, cleared once focus is taken).
  bool renameFocusPending_ = false;
};

}  // namespace donner::editor
