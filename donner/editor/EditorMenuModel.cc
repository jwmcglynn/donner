#include "donner/editor/EditorMenuModel.h"

#include <array>

namespace donner::editor {
namespace {

constexpr EditorMenuNode Item(std::string_view label, std::string_view shortcut,
                              MenuBarCommand command) {
  return {.kind = EditorMenuNode::Kind::Command,
          .label = label,
          .shortcut = shortcut,
          .command = command};
}

constexpr EditorMenuNode Separator() {
  return {.kind = EditorMenuNode::Kind::Separator};
}

constexpr EditorMenuNode Submenu(std::string_view label, std::span<const EditorMenuNode> children) {
  return {.kind = EditorMenuNode::Kind::Submenu, .label = label, .children = children};
}

constexpr std::array kCompositedItems = {
    Item("On", "", MenuBarCommand::SetCompositedRenderingOn),
    Item("Filters Only", "", MenuBarCommand::SetCompositedRenderingFilterOnly),
    Item("Off", "", MenuBarCommand::SetCompositedRenderingOff),
};

constexpr std::array kPerformanceItems = {
    Item("Off", "", MenuBarCommand::SetPerfOverlayOff),
    Item("FPS Pill", "", MenuBarCommand::SetPerfOverlayFpsPill),
    Item("Full Graph", "", MenuBarCommand::SetPerfOverlayFullGraph),
};

constexpr std::array kDonnerItems = {
    Item("About...", "", MenuBarCommand::OpenAbout),
    Item("Quit Donner", "Cmd+Q", MenuBarCommand::Quit),
};

constexpr std::array kFileItems = {
    Item("New", "Cmd+N", MenuBarCommand::NewFile),
    Item("Open...", "Cmd+O", MenuBarCommand::OpenFile),
    Item("Open Sample...", "", MenuBarCommand::OpenSamples),
    Item("Save", "Cmd+S", MenuBarCommand::SaveFile),
    Item("Save As...", "Cmd+Shift+S", MenuBarCommand::SaveFileAs),
    Separator(),
    Item("Export Viewport as SVG...", "", MenuBarCommand::ExportViewportSvg),
    Item("Export Viewport as SVG (with overlay)...", "",
         MenuBarCommand::ExportViewportSvgWithOverlay),
    Separator(),
    Item("Revert", "", MenuBarCommand::RevertFile),
};

constexpr std::array kEditItems = {
    Item("Undo", "Cmd+Z", MenuBarCommand::Undo),
    Item("Redo", "Cmd+Shift+Z", MenuBarCommand::Redo),
    Separator(),
    Item("Cut", "Cmd+X", MenuBarCommand::Cut),
    Item("Copy", "Cmd+C", MenuBarCommand::Copy),
    Item("Paste", "Cmd+V", MenuBarCommand::Paste),
    Item("Paste in Front", "Cmd+F", MenuBarCommand::PasteInFront),
    Separator(),
    Item("Convert Text to Outlines", "", MenuBarCommand::ConvertTextToOutlines),
    Item("Group", "Cmd+G", MenuBarCommand::Group),
    Item("Ungroup", "Cmd+Shift+G", MenuBarCommand::Ungroup),
    Item("Select All", "Cmd+A", MenuBarCommand::SelectAll),
    Item("Deselect All", "Cmd+Shift+A", MenuBarCommand::DeselectAll),
};

constexpr std::array kViewItems = {
    Item("Source Focus Mode", "Cmd+Enter", MenuBarCommand::ToggleSourceFocusMode),
    Separator(),
    Item("Zoom In", "Cmd+=", MenuBarCommand::ZoomIn),
    Item("Zoom Out", "Cmd+-", MenuBarCommand::ZoomOut),
    Item("Actual Size", "Cmd+0", MenuBarCommand::ActualSize),
    Separator(),
    Item("Compositor Debug Info", "", MenuBarCommand::ToggleCompositorDebugPanel),
    Item("Compositor Tile Overlay", "", MenuBarCommand::ToggleCompositorTileOverlay),
    Item("Geometry Debug Overlay", "", MenuBarCommand::ToggleGeometryDebugOverlay),
    Separator(),
    Item("Lock Panel Layout", "", MenuBarCommand::ToggleLayoutLock),
    Item("Reset Layout", "", MenuBarCommand::ResetLayout),
    Submenu("Composited Rendering", kCompositedItems),
    Submenu("Performance Overlay", kPerformanceItems),
};

constexpr std::array kMenus = {
    EditorMenu{"DONNER", kDonnerItems},
    EditorMenu{"File", kFileItems},
    EditorMenu{"Edit", kEditItems},
    EditorMenu{"View", kViewItems},
};

}  // namespace

std::span<const EditorMenu> EditorMenuModel() {
  return kMenus;
}

bool SuppressNativeMenuShortcuts(const MenuBarState& state) {
  return (state.textToolEditing && !state.sourcePaneFocused) || state.inspectorTextInputFocused;
}

EditorMenuItemPresentation PresentEditorMenuItem(const EditorMenuNode& item,
                                                 const MenuBarState& state,
                                                 EditorMenuSurface surface) {
  EditorMenuItemPresentation result;
  if (item.kind != EditorMenuNode::Kind::Command) {
    return result;
  }
  if (surface == EditorMenuSurface::Native && SuppressNativeMenuShortcuts(state)) {
    switch (item.command) {
      case MenuBarCommand::Undo:
      case MenuBarCommand::Redo:
      case MenuBarCommand::Cut:
      case MenuBarCommand::Copy:
      case MenuBarCommand::Paste:
      case MenuBarCommand::PasteInFront:
      case MenuBarCommand::ConvertTextToOutlines:
      case MenuBarCommand::Group:
      case MenuBarCommand::Ungroup:
      case MenuBarCommand::SelectAll:
      case MenuBarCommand::DeselectAll: result.enabled = false; return result;
      default: break;
    }
  }
  // Canvas-only actions should not mutate a shape behind the source editor.
  // Apply this to both menu surfaces so their enabled states stay in sync.
  if (state.sourcePaneFocused) {
    switch (item.command) {
      case MenuBarCommand::PasteInFront:
      case MenuBarCommand::ConvertTextToOutlines:
      case MenuBarCommand::Group:
      case MenuBarCommand::Ungroup: result.enabled = false; return result;
      default: break;
    }
  }
  switch (item.command) {
    case MenuBarCommand::SaveFile:
    case MenuBarCommand::SaveFileAs:
    case MenuBarCommand::ExportViewportSvg:
    case MenuBarCommand::ExportViewportSvgWithOverlay: result.enabled = state.canSave; break;
    case MenuBarCommand::RevertFile: result.enabled = state.canRevert; break;
    case MenuBarCommand::Undo: result.enabled = state.canUndo; break;
    case MenuBarCommand::Redo: result.enabled = state.canRedo; break;
    case MenuBarCommand::Cut:
    case MenuBarCommand::Copy:
      result.enabled = state.sourcePaneFocused || state.hasShapeSelection;
      break;
    case MenuBarCommand::Paste:
      result.enabled = state.sourcePaneFocused || state.hasShapeClipboard;
      break;
    case MenuBarCommand::PasteInFront: result.enabled = state.hasShapeClipboard; break;
    case MenuBarCommand::ConvertTextToOutlines: result.enabled = state.hasTextSelection; break;
    case MenuBarCommand::Group: result.enabled = state.canGroup; break;
    case MenuBarCommand::Ungroup: result.enabled = state.canUngroup; break;
    case MenuBarCommand::SelectAll:
      result.enabled = state.sourcePaneFocused || state.hasSelectableElements;
      break;
    case MenuBarCommand::DeselectAll:
      result.enabled = state.sourcePaneFocused || state.hasShapeSelection;
      break;
    case MenuBarCommand::ToggleSourceFocusMode: result.checked = state.sourceFocusMode; break;
    case MenuBarCommand::ToggleCompositorDebugPanel:
      result.checked = state.showCompositorDebugPanel;
      break;
    case MenuBarCommand::ToggleCompositorTileOverlay:
      result.checked = state.compositorTileOverlay;
      break;
    case MenuBarCommand::ToggleGeometryDebugOverlay:
      result.checked = state.geometryDebugOverlay;
      break;
    case MenuBarCommand::ToggleLayoutLock: result.checked = state.panelLayoutLocked; break;
    case MenuBarCommand::SetPerfOverlayOff:
      result.checked = state.perfOverlayMode == PerfOverlayMode::Off;
      break;
    case MenuBarCommand::SetPerfOverlayFpsPill:
      result.checked = state.perfOverlayMode == PerfOverlayMode::FpsPill;
      break;
    case MenuBarCommand::SetPerfOverlayFullGraph:
      result.checked = state.perfOverlayMode == PerfOverlayMode::FullGraph;
      break;
    case MenuBarCommand::SetCompositedRenderingOff:
      result.checked = state.compositedRenderingMode == CompositedRenderingMode::Off;
      break;
    case MenuBarCommand::SetCompositedRenderingFilterOnly:
      result.checked = state.compositedRenderingMode == CompositedRenderingMode::FilterOnly;
      break;
    case MenuBarCommand::SetCompositedRenderingOn:
      result.checked = state.compositedRenderingMode == CompositedRenderingMode::On;
      break;
    default: break;
  }
  return result;
}

bool NativeMenuActivationAllowed(const EditorMenuNode& item, const MenuBarState& state,
                                 bool keyEquivalent) {
  return !(keyEquivalent && SuppressNativeMenuShortcuts(state)) &&
         PresentEditorMenuItem(item, state, EditorMenuSurface::Native).enabled;
}

}  // namespace donner::editor
