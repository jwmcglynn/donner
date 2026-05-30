#include "donner/editor/MenuBarPresenter.h"

#include <utility>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

MenuBarActions MenuBarPresenter::render(const MenuBarState& state, ImFont* boldMenuFont) const {
  MenuBarActions actions;

  if (!ImGui::BeginMainMenuBar()) {
    return actions;
  }

  const bool pushedBoldDonnerMenuFont = boldMenuFont != nullptr;
  if (pushedBoldDonnerMenuFont) {
    ImGui::PushFont(boldMenuFont);
  }
  const bool donnerMenuOpen = ImGui::BeginMenu("Donner SVG Editor");
  if (pushedBoldDonnerMenuFont) {
    ImGui::PopFont();
  }
  if (donnerMenuOpen) {
    if (ImGui::MenuItem("About...")) {
      actions.openAbout = true;
    }
    if (ImGui::MenuItem("Quit Donner", "Cmd+Q")) {
      actions.quit = true;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open...", "Cmd+O")) {
      actions.openFile = true;
    }
    if (ImGui::MenuItem("Save", "Cmd+S", false, state.canSave)) {
      actions.saveFile = true;
    }
    if (ImGui::MenuItem("Save As...", "Cmd+Shift+S", false, state.canSave)) {
      actions.saveFileAs = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Revert", nullptr, false, state.canRevert)) {
      actions.revertFile = true;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Undo", "Cmd+Z", false, state.canUndo)) {
      actions.undo = true;
    }
    if (ImGui::MenuItem("Redo", "Cmd+Shift+Z", false, state.canRedo)) {
      actions.redo = true;
    }
    ImGui::Separator();

    if (!state.sourcePaneFocused) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Cut", "Cmd+X", false, state.sourcePaneFocused)) {
      actions.cut = true;
    }
    if (ImGui::MenuItem("Copy", "Cmd+C", false, state.sourcePaneFocused)) {
      actions.copy = true;
    }
    if (ImGui::MenuItem("Paste", "Cmd+V", false, state.sourcePaneFocused)) {
      actions.paste = true;
    }
    if (ImGui::MenuItem("Select All", "Cmd+A", false, state.sourcePaneFocused)) {
      actions.selectAll = true;
    }
    if (!state.sourcePaneFocused) {
      ImGui::EndDisabled();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Source Focus Mode", "Cmd+Enter", state.sourceFocusMode)) {
      actions.toggleSourceFocusMode = true;
    }
    if (ImGui::MenuItem("Compositor Debug", nullptr, state.compositorDebugVisible)) {
      actions.toggleCompositorDebug = true;
    }
    if (ImGui::MenuItem("Performance Overlays", nullptr, state.perfOverlaysVisible)) {
      actions.togglePerfOverlays = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Zoom In", "Cmd+=")) {
      actions.zoomIn = true;
    }
    if (ImGui::MenuItem("Zoom Out", "Cmd+-")) {
      actions.zoomOut = true;
    }
    if (ImGui::MenuItem("Actual Size", "Cmd+0")) {
      actions.actualSize = true;
    }
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
  return actions;
}

}  // namespace donner::editor
