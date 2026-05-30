#pragma once
/// @file

struct ImFont;

namespace donner::editor {

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
  /// the precondition for "Convert Text to Outlines" (design doc 0047 M5).
  bool hasTextSelection = false;
  /// Current visibility of the Compositor Debug panel (drives the View-menu
  /// checkmark). Off by default.
  bool showCompositorDebugPanel = false;
  /// Current visibility of the render-pane performance overlay (drives the
  /// View-menu checkmark). On by default.
  bool showPerfOverlay = true;
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
  bool selectAll = false;
  bool deselectAll = false;
  bool zoomIn = false;
  bool zoomOut = false;
  bool actualSize = false;
  bool toggleSourceFocusMode = false;
  /// Set when the user toggles the Compositor Debug panel via the View menu.
  bool toggleCompositorDebugPanel = false;
  /// Set when the user toggles the performance overlay via the View menu.
  bool togglePerfOverlay = false;
};

/// Renders the app's top menu bar and reports semantic actions back to the shell.
class MenuBarPresenter {
public:
  [[nodiscard]] MenuBarActions render(const MenuBarState& state, ImFont* boldMenuFont) const;
};

}  // namespace donner::editor
