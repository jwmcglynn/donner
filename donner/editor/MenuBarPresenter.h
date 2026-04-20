#pragma once
/// @file

struct ImFont;

namespace donner::editor {

struct MenuBarState {
  bool sourcePaneFocused = false;
  bool canUndo = false;
  bool canRedo = false;
  bool experimentalMode = false;
  bool canToggleCompositedRendering = false;
  /// Current state of the tight-bounded-segments compositor gate.
  /// Rendered under View → "Tight-Bounded Segments (debug)". Flipping it
  /// off is the bisection knob for design doc 0027 regressions.
  bool tightBoundedSegmentsEnabled = true;
};

struct MenuBarActions {
  bool openAbout = false;
  bool openFile = false;
  bool quit = false;
  bool undo = false;
  bool redo = false;
  bool cut = false;
  bool copy = false;
  bool paste = false;
  bool selectAll = false;
  bool zoomIn = false;
  bool zoomOut = false;
  bool actualSize = false;
  bool toggleCompositedRendering = false;
  bool toggleTightBoundedSegments = false;
};

/// Renders the app's top menu bar and reports semantic actions back to the shell.
class MenuBarPresenter {
public:
  [[nodiscard]] MenuBarActions render(const MenuBarState& state, ImFont* boldMenuFont) const;
};

}  // namespace donner::editor
