#include "donner/editor/MenuBarPresenter.h"

#include <utility>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

void ApplyMenuBarCommand(bool activated, MenuBarCommand command, const MenuBarState& state,
                         MenuBarActions* actions) {
  if (!activated || actions == nullptr) {
    return;
  }

  switch (command) {
    case MenuBarCommand::OpenAbout: actions->openAbout = true; return;
    case MenuBarCommand::OpenFile: actions->openFile = true; return;
    case MenuBarCommand::SaveFile: actions->saveFile = true; return;
    case MenuBarCommand::SaveFileAs: actions->saveFileAs = true; return;
    case MenuBarCommand::ExportViewportSvg: actions->exportViewportSvg = true; return;
    case MenuBarCommand::ExportViewportSvgWithOverlay:
      actions->exportViewportSvgWithOverlay = true;
      return;
    case MenuBarCommand::RevertFile: actions->revertFile = true; return;
    case MenuBarCommand::Quit: actions->quit = true; return;
    case MenuBarCommand::Undo: actions->undo = true; return;
    case MenuBarCommand::Redo: actions->redo = true; return;
    case MenuBarCommand::Cut: actions->cut = true; return;
    case MenuBarCommand::Copy: actions->copy = true; return;
    case MenuBarCommand::Paste: actions->paste = true; return;
    case MenuBarCommand::PasteInFront: actions->pasteInFront = true; return;
    case MenuBarCommand::ConvertTextToOutlines: actions->convertTextToOutlines = true; return;
    case MenuBarCommand::SelectAll:
      if (state.sourcePaneFocused) {
        actions->selectAll = true;
      } else {
        actions->selectAllCanvas = true;
      }
      return;
    case MenuBarCommand::DeselectAll:
      if (state.sourcePaneFocused) {
        actions->deselectAll = true;
      } else {
        actions->deselectAllCanvas = true;
      }
      return;
    case MenuBarCommand::ZoomIn: actions->zoomIn = true; return;
    case MenuBarCommand::ZoomOut: actions->zoomOut = true; return;
    case MenuBarCommand::ActualSize: actions->actualSize = true; return;
    case MenuBarCommand::ToggleSourceFocusMode: actions->toggleSourceFocusMode = true; return;
    case MenuBarCommand::ToggleCompositorDebugPanel:
      actions->toggleCompositorDebugPanel = true;
      return;
    case MenuBarCommand::ToggleCompositorTileOverlay:
      actions->toggleCompositorTileOverlay = true;
      return;
    case MenuBarCommand::ToggleGeometryDebugOverlay:
      actions->toggleGeometryDebugOverlay = true;
      return;
    case MenuBarCommand::SetPerfOverlayOff:
      actions->setPerfOverlayMode = true;
      actions->perfOverlayMode = PerfOverlayMode::Off;
      return;
    case MenuBarCommand::SetPerfOverlayFpsPill:
      actions->setPerfOverlayMode = true;
      actions->perfOverlayMode = PerfOverlayMode::FpsPill;
      return;
    case MenuBarCommand::SetPerfOverlayFullGraph:
      actions->setPerfOverlayMode = true;
      actions->perfOverlayMode = PerfOverlayMode::FullGraph;
      return;
    case MenuBarCommand::ToggleLayoutLock: actions->toggleLayoutLock = true; return;
    case MenuBarCommand::ResetLayout: actions->resetLayout = true; return;
  }
}

void ApplyViewMenuToggleActions(const MenuBarActions& actions, bool* showCompositorDebugPanel,
                                PerfOverlayMode* perfOverlayMode, bool* geometryDebugOverlay,
                                bool* compositorTileOverlay) {
  if (actions.toggleCompositorDebugPanel && showCompositorDebugPanel != nullptr) {
    *showCompositorDebugPanel = !*showCompositorDebugPanel;
  }
  if (actions.setPerfOverlayMode && perfOverlayMode != nullptr) {
    *perfOverlayMode = actions.perfOverlayMode;
  }
  if (actions.toggleGeometryDebugOverlay && geometryDebugOverlay != nullptr) {
    *geometryDebugOverlay = !*geometryDebugOverlay;
  }
  if (actions.toggleCompositorTileOverlay && compositorTileOverlay != nullptr) {
    *compositorTileOverlay = !*compositorTileOverlay;
  }
}

MenuBarActions MenuBarPresenter::render(const MenuBarState& state, ImFont* boldMenuFont) const {
  MenuBarActions actions;

  if (!ImGui::BeginMainMenuBar()) {
    return actions;
  }

  const EditorTheme& theme = EditorTheme::Active();
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImVec2 brandOrigin = ImGui::GetCursorScreenPos();
  const float markerHeight = ImGui::GetFrameHeight() - theme.space2;
  drawList->AddRectFilled(
      ImVec2(brandOrigin.x, brandOrigin.y + theme.space1),
      ImVec2(brandOrigin.x + theme.space1, brandOrigin.y + theme.space1 + markerHeight),
      theme.accentDefault, 2.0f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme.space3);

  const bool pushedBoldDonnerMenuFont = boldMenuFont != nullptr;
  if (pushedBoldDonnerMenuFont) {
    ImGui::PushFont(boldMenuFont);
  }
  const bool donnerMenuOpen = ImGui::BeginMenu("DONNER");
  if (pushedBoldDonnerMenuFont) {
    ImGui::PopFont();
  }
  if (donnerMenuOpen) {
    ApplyMenuBarCommand(ImGui::MenuItem("About..."), MenuBarCommand::OpenAbout, state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Quit Donner", "Cmd+Q"), MenuBarCommand::Quit, state,
                        &actions);
    ImGui::EndMenu();
  }

  ImGui::SameLine(0.0f, theme.space1);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "SVG EDITOR");
  ImGui::SameLine(0.0f, theme.space4);

  if (ImGui::BeginMenu("File")) {
    ApplyMenuBarCommand(ImGui::MenuItem("Open...", "Cmd+O"), MenuBarCommand::OpenFile, state,
                        &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Save", "Cmd+S", false, state.canSave),
                        MenuBarCommand::SaveFile, state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Save As...", "Cmd+Shift+S", false, state.canSave),
                        MenuBarCommand::SaveFileAs, state, &actions);
    ImGui::Separator();
    ApplyMenuBarCommand(ImGui::MenuItem("Export Viewport as SVG...", nullptr, false, state.canSave),
                        MenuBarCommand::ExportViewportSvg, state, &actions);
    ApplyMenuBarCommand(
        ImGui::MenuItem("Export Viewport as SVG (with overlay)...", nullptr, false, state.canSave),
        MenuBarCommand::ExportViewportSvgWithOverlay, state, &actions);
    ImGui::Separator();
    ApplyMenuBarCommand(ImGui::MenuItem("Revert", nullptr, false, state.canRevert),
                        MenuBarCommand::RevertFile, state, &actions);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    ApplyMenuBarCommand(ImGui::MenuItem("Undo", "Cmd+Z", false, state.canUndo),
                        MenuBarCommand::Undo, state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Redo", "Cmd+Shift+Z", false, state.canRedo),
                        MenuBarCommand::Redo, state, &actions);
    ImGui::Separator();

    // Cut/Copy act on the source pane when it is focused, otherwise on the
    // canvas shape selection. Paste / Paste in Front likewise fall back to the
    // shape clipboard. Each item enables on the relevant precondition.
    const bool canCutCopy = state.sourcePaneFocused || state.hasShapeSelection;
    const bool canPaste = state.sourcePaneFocused || state.hasShapeClipboard;
    ApplyMenuBarCommand(ImGui::MenuItem("Cut", "Cmd+X", false, canCutCopy), MenuBarCommand::Cut,
                        state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Copy", "Cmd+C", false, canCutCopy), MenuBarCommand::Copy,
                        state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Paste", "Cmd+V", false, canPaste), MenuBarCommand::Paste,
                        state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Paste in Front", "Cmd+F", false, state.hasShapeClipboard),
                        MenuBarCommand::PasteInFront, state, &actions);
    ImGui::Separator();
    ApplyMenuBarCommand(
        ImGui::MenuItem("Convert Text to Outlines", nullptr, false, state.hasTextSelection),
        MenuBarCommand::ConvertTextToOutlines, state, &actions);
    // "Select All" routes to the source/XML pane's text Select-All while it owns keyboard focus,
    // and otherwise to the canvas Select-All (every selectable element). Enabled when either path
    // can act.
    ApplyMenuBarCommand(ImGui::MenuItem("Select All", "Cmd+A", false,
                                        state.sourcePaneFocused || state.hasSelectableElements),
                        MenuBarCommand::SelectAll, state, &actions);
    // "Deselect All" routes to the source/XML pane's text deselect (collapse selection to caret)
    // while it owns keyboard focus, and otherwise clears the canvas selection. Enabled when either
    // path can act: the source pane can always collapse its caret, and the canvas needs a
    // selection. Cmd+Shift+A mirrors the focus-aware shortcut handled in EditorShell.
    ApplyMenuBarCommand(ImGui::MenuItem("Deselect All", "Cmd+Shift+A", false,
                                        state.sourcePaneFocused || state.hasShapeSelection),
                        MenuBarCommand::DeselectAll, state, &actions);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ApplyMenuBarCommand(ImGui::MenuItem("Source Focus Mode", "Cmd+Enter", state.sourceFocusMode),
                        MenuBarCommand::ToggleSourceFocusMode, state, &actions);
    ImGui::Separator();
    ApplyMenuBarCommand(ImGui::MenuItem("Zoom In", "Cmd+="), MenuBarCommand::ZoomIn, state,
                        &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Zoom Out", "Cmd+-"), MenuBarCommand::ZoomOut, state,
                        &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Actual Size", "Cmd+0"), MenuBarCommand::ActualSize, state,
                        &actions);
    ImGui::Separator();
    ApplyMenuBarCommand(
        ImGui::MenuItem("Compositor Debug", nullptr, state.showCompositorDebugPanel),
        MenuBarCommand::ToggleCompositorDebugPanel, state, &actions);
    ApplyMenuBarCommand(
        ImGui::MenuItem("Compositor Tile Overlay", nullptr, state.compositorTileOverlay),
        MenuBarCommand::ToggleCompositorTileOverlay, state, &actions);
    ApplyMenuBarCommand(
        ImGui::MenuItem("Geometry Debug Overlay", nullptr, state.geometryDebugOverlay),
        MenuBarCommand::ToggleGeometryDebugOverlay, state, &actions);
    ImGui::Separator();
    ApplyMenuBarCommand(ImGui::MenuItem("Lock Panel Layout", nullptr, state.panelLayoutLocked),
                        MenuBarCommand::ToggleLayoutLock, state, &actions);
    ApplyMenuBarCommand(ImGui::MenuItem("Reset Layout"), MenuBarCommand::ResetLayout, state,
                        &actions);
    if (ImGui::BeginMenu("Performance Overlay")) {
      ApplyMenuBarCommand(
          ImGui::MenuItem("Off", nullptr, state.perfOverlayMode == PerfOverlayMode::Off),
          MenuBarCommand::SetPerfOverlayOff, state, &actions);
      ApplyMenuBarCommand(
          ImGui::MenuItem("FPS Pill", nullptr, state.perfOverlayMode == PerfOverlayMode::FpsPill),
          MenuBarCommand::SetPerfOverlayFpsPill, state, &actions);
      ApplyMenuBarCommand(ImGui::MenuItem("Full Graph", nullptr,
                                          state.perfOverlayMode == PerfOverlayMode::FullGraph),
                          MenuBarCommand::SetPerfOverlayFullGraph, state, &actions);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  const ImVec2 barPosition = ImGui::GetWindowPos();
  const ImVec2 barSize = ImGui::GetWindowSize();
  drawList->AddLine(ImVec2(barPosition.x, barPosition.y + barSize.y - 1.0f),
                    ImVec2(barPosition.x + barSize.x, barPosition.y + barSize.y - 1.0f),
                    theme.borderSubtle);
  ImGui::EndMainMenuBar();
  return actions;
}

}  // namespace donner::editor
