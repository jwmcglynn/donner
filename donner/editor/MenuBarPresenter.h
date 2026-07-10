#pragma once
/// @file

#include <cstdint>

struct ImFont;

namespace donner::editor {

/// Display mode for the render-pane performance overlay, selected via the
/// View menu's "Performance Overlay" submenu.
enum class PerfOverlayMode : std::uint8_t {
  /// No performance UI is drawn over the render pane.
  Off,
  /// Compact rounded chip showing only smoothed FPS and frame ms.
  FpsPill,
  /// Full stacked frame-cost graph plus the presentation-memory graph.
  FullGraph,
};

struct MenuBarState {
  bool sourcePaneFocused = false;
  bool canSave = false;
  bool canRevert = false;
  bool canUndo = false;
  bool canRedo = false;
  bool sourceFocusMode = true;
  /// True when the canvas has one or more selected shapes. Enables shape
  /// Cut/Copy when the source pane is not focused.
  bool hasShapeSelection = false;
  /// True when the shape clipboard holds a paste-able payload. Enables shape
  /// Paste / Paste in Front when the source pane is not focused.
  bool hasShapeClipboard = false;
  /// True when the canvas selection is exactly one or more `<text>` elements,
  /// the precondition for "Convert Text to Outlines".
  bool hasTextSelection = false;
  /// True when the document has at least one selectable element. Enables the canvas "Select All"
  /// when the source pane is not focused.
  bool hasSelectableElements = false;
  /// Current visibility of the Compositor Debug panel (drives the View-menu
  /// checkmark). Off by default.
  bool showCompositorDebugPanel = false;
  /// Whether compositor tile boundaries and identities are drawn directly over the canvas.
  bool compositorTileOverlay = false;
  /// Whether the Geode geometry debug overlay (band strips + per-path
  /// bounding-quad triangles) is enabled on the document renderer
  /// (drives the View-menu checkmark). Off by default.
  bool geometryDebugOverlay = false;
  /// Current render-pane performance overlay mode (drives the View-menu
  /// checkmarks). Off by default.
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;
  /// Whether the dockable panel layout is locked (drives the View-menu "Lock
  /// Panel Layout" checkmark). Locked by default.
  bool panelLayoutLocked = true;
};

struct MenuBarActions {
  bool openAbout = false;
  bool openFile = false;
  bool saveFile = false;
  bool saveFileAs = false;
  bool exportViewportSvg = false;
  bool exportViewportSvgWithOverlay = false;
  bool revertFile = false;
  bool quit = false;
  bool undo = false;
  bool redo = false;
  bool cut = false;
  bool copy = false;
  bool paste = false;
  bool pasteInFront = false;
  bool convertTextToOutlines = false;
  /// Text Select-All in the source/XML pane (fires when the source pane owns keyboard focus).
  bool selectAll = false;
  /// Canvas Select-All - selects every selectable element (fires when the source pane is not
  /// focused).
  bool selectAllCanvas = false;
  /// Text Deselect in the source/XML pane - collapses the text selection to the caret (fires when
  /// the source pane owns keyboard focus).
  bool deselectAll = false;
  /// Canvas Deselect-All - clears the canvas selection (fires when the source pane is not focused).
  bool deselectAllCanvas = false;
  bool zoomIn = false;
  bool zoomOut = false;
  bool actualSize = false;
  bool toggleSourceFocusMode = false;
  /// Set when the user toggles the Compositor Debug panel via the View menu.
  bool toggleCompositorDebugPanel = false;
  /// Set when the user toggles compositor tile boundaries over the canvas.
  bool toggleCompositorTileOverlay = false;
  /// Set when the user toggles the Geode geometry debug overlay via the
  /// View menu.
  bool toggleGeometryDebugOverlay = false;
  /// Set when the user picks a performance overlay mode via the View menu.
  bool setPerfOverlayMode = false;
  /// Requested performance overlay mode; meaningful only while
  /// `setPerfOverlayMode` is true.
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;
  /// Set when the user toggles the panel-layout lock via the View menu.
  bool toggleLayoutLock = false;
  /// Set when the user picks "Reset Layout" to restore the default dock layout.
  bool resetLayout = false;
};

/// Semantic command emitted by a top-level menu item.
enum class MenuBarCommand {
  OpenAbout,
  OpenFile,
  SaveFile,
  SaveFileAs,
  ExportViewportSvg,
  ExportViewportSvgWithOverlay,
  RevertFile,
  Quit,
  Undo,
  Redo,
  Cut,
  Copy,
  Paste,
  PasteInFront,
  ConvertTextToOutlines,
  SelectAll,
  DeselectAll,
  ZoomIn,
  ZoomOut,
  ActualSize,
  ToggleSourceFocusMode,
  ToggleCompositorDebugPanel,
  ToggleCompositorTileOverlay,
  ToggleGeometryDebugOverlay,
  SetPerfOverlayOff,
  SetPerfOverlayFpsPill,
  SetPerfOverlayFullGraph,
  ToggleLayoutLock,
  ResetLayout,
};

/// Apply an activated semantic menu command to an action accumulator.
///
/// @param activated True when the menu item was clicked.
/// @param command Semantic command represented by the menu item.
/// @param state Menu state used by focus-dependent commands.
/// @param actions Action accumulator to update.
void ApplyMenuBarCommand(bool activated, MenuBarCommand command, const MenuBarState& state,
                         MenuBarActions* actions);

/// Apply View-menu visibility toggle actions to persistent UI state.
///
/// @param actions Edge-triggered menu actions from \ref MenuBarPresenter::render.
/// @param showCompositorDebugPanel Current Compositor Debug panel visibility.
/// @param perfOverlayMode Current performance overlay mode.
/// @param geometryDebugOverlay Current Geode geometry debug overlay state.
///   Optional (may be null) so callers without a document renderer skip it.
void ApplyViewMenuToggleActions(const MenuBarActions& actions, bool* showCompositorDebugPanel,
                                PerfOverlayMode* perfOverlayMode,
                                bool* geometryDebugOverlay = nullptr,
                                bool* compositorTileOverlay = nullptr);

/// Renders the app's top menu bar and reports semantic actions back to the shell.
class MenuBarPresenter {
public:
  [[nodiscard]] MenuBarActions render(const MenuBarState& state, ImFont* boldMenuFont) const;
};

}  // namespace donner::editor
