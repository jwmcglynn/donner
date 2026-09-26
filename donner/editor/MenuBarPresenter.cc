#include "donner/editor/MenuBarPresenter.h"

#include <utility>

#include "donner/editor/EditorMenuModel.h"
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
    case MenuBarCommand::NewFile: actions->newFile = true; return;
    case MenuBarCommand::OpenFile: actions->openFile = true; return;
    case MenuBarCommand::OpenSamples: actions->openSamples = true; return;
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
    case MenuBarCommand::Group: actions->group = true; return;
    case MenuBarCommand::Ungroup: actions->ungroup = true; return;
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
    case MenuBarCommand::SetCompositedRenderingOff:
      actions->setCompositedRenderingMode = true;
      actions->compositedRenderingMode = CompositedRenderingMode::Off;
      return;
    case MenuBarCommand::SetCompositedRenderingFilterOnly:
      actions->setCompositedRenderingMode = true;
      actions->compositedRenderingMode = CompositedRenderingMode::FilterOnly;
      return;
    case MenuBarCommand::SetCompositedRenderingOn:
      actions->setCompositedRenderingMode = true;
      actions->compositedRenderingMode = CompositedRenderingMode::On;
      return;
    case MenuBarCommand::ToggleLayoutLock: actions->toggleLayoutLock = true; return;
    case MenuBarCommand::ResetLayout: actions->resetLayout = true; return;
  }
}

void ApplyViewMenuToggleActions(const MenuBarActions& actions, bool* showCompositorDebugPanel,
                                PerfOverlayMode* perfOverlayMode, bool* geometryDebugOverlay,
                                bool* compositorTileOverlay,
                                CompositedRenderingMode* compositedRenderingMode) {
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
  if (actions.setCompositedRenderingMode && compositedRenderingMode != nullptr) {
    *compositedRenderingMode = actions.compositedRenderingMode;
  }
}

namespace {

void RenderMenuNodes(std::span<const EditorMenuNode> nodes, const MenuBarState& state,
                     MenuBarActions* actions) {
  for (const EditorMenuNode& node : nodes) {
    if (node.kind == EditorMenuNode::Kind::Separator) {
      ImGui::Separator();
      continue;
    }
    if (node.kind == EditorMenuNode::Kind::Submenu) {
      if (ImGui::BeginMenu(node.label.data())) {
        RenderMenuNodes(node.children, state, actions);
        ImGui::EndMenu();
      }
      continue;
    }
    const EditorMenuItemPresentation presentation = PresentEditorMenuItem(node, state);
    const char* shortcut = node.shortcut.empty() ? nullptr : node.shortcut.data();
    const bool activated =
        ImGui::MenuItem(node.label.data(), shortcut, presentation.checked, presentation.enabled);
    ApplyMenuBarCommand(activated, node.command, state, actions);
  }
}

}  // namespace

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

  for (const EditorMenu& menu : EditorMenuModel()) {
    const bool brandMenu = menu.title == "DONNER";
    if (brandMenu && boldMenuFont != nullptr) {
      ImGui::PushFont(boldMenuFont);
    }
    const bool open = ImGui::BeginMenu(menu.title.data());
    if (brandMenu && boldMenuFont != nullptr) {
      ImGui::PopFont();
    }
    if (open) {
      RenderMenuNodes(menu.items, state, &actions);
      ImGui::EndMenu();
    }
    if (brandMenu) {
      ImGui::SameLine(0.0f, theme.space1);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "SVG EDITOR");
      ImGui::SameLine(0.0f, theme.space4);
    }
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
