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
  bool selectAll = false;
  bool zoomIn = false;
  bool zoomOut = false;
  bool actualSize = false;
  bool toggleSourceFocusMode = false;
};

/// Renders the app's top menu bar and reports semantic actions back to the shell.
class MenuBarPresenter {
public:
  [[nodiscard]] MenuBarActions render(const MenuBarState& state, ImFont* boldMenuFont) const;
};

}  // namespace donner::editor
