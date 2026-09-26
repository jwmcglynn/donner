#include "donner/editor/EditorMenuModel.h"

#include <array>
#include <optional>

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

bool NativeTextCaptureDisables(MenuBarCommand command) {
  switch (command) {
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
    case MenuBarCommand::DeselectAll: return true;
    default: return false;
  }
}

bool SourceFocusDisables(MenuBarCommand command) {
  switch (command) {
    case MenuBarCommand::PasteInFront:
    case MenuBarCommand::ConvertTextToOutlines:
    case MenuBarCommand::Group:
    case MenuBarCommand::Ungroup: return true;
    default: return false;
  }
}

std::optional<bool> FileAndHistoryEnabled(MenuBarCommand command, const MenuBarState& state) {
  switch (command) {
    case MenuBarCommand::SaveFile:
    case MenuBarCommand::SaveFileAs:
    case MenuBarCommand::ExportViewportSvg:
    case MenuBarCommand::ExportViewportSvgWithOverlay: return state.canSave;
    case MenuBarCommand::RevertFile: return state.canRevert;
    case MenuBarCommand::Undo: return state.canUndo;
    case MenuBarCommand::Redo: return state.canRedo;
    default: return std::nullopt;
  }
}

std::optional<bool> ClipboardEnabled(MenuBarCommand command, const MenuBarState& state) {
  switch (command) {
    case MenuBarCommand::Cut:
    case MenuBarCommand::Copy: return state.sourcePaneFocused || state.hasShapeSelection;
    case MenuBarCommand::Paste: return state.sourcePaneFocused || state.hasShapeClipboard;
    case MenuBarCommand::PasteInFront: return state.hasShapeClipboard;
    default: return std::nullopt;
  }
}

std::optional<bool> SelectionEnabled(MenuBarCommand command, const MenuBarState& state) {
  switch (command) {
    case MenuBarCommand::ConvertTextToOutlines: return state.hasTextSelection;
    case MenuBarCommand::Group: return state.canGroup;
    case MenuBarCommand::Ungroup: return state.canUngroup;
    case MenuBarCommand::SelectAll: return state.sourcePaneFocused || state.hasSelectableElements;
    case MenuBarCommand::DeselectAll: return state.sourcePaneFocused || state.hasShapeSelection;
    default: return std::nullopt;
  }
}

bool CommandEnabled(MenuBarCommand command, const MenuBarState& state) {
  if (const std::optional<bool> enabled = FileAndHistoryEnabled(command, state)) {
    return *enabled;
  }
  if (const std::optional<bool> enabled = ClipboardEnabled(command, state)) {
    return *enabled;
  }
  if (const std::optional<bool> enabled = SelectionEnabled(command, state)) {
    return *enabled;
  }
  return true;
}

std::optional<bool> ViewCommandChecked(MenuBarCommand command, const MenuBarState& state) {
  switch (command) {
    case MenuBarCommand::ToggleSourceFocusMode: return state.sourceFocusMode;
    case MenuBarCommand::ToggleCompositorDebugPanel: return state.showCompositorDebugPanel;
    case MenuBarCommand::ToggleCompositorTileOverlay: return state.compositorTileOverlay;
    case MenuBarCommand::ToggleGeometryDebugOverlay: return state.geometryDebugOverlay;
    case MenuBarCommand::ToggleLayoutLock: return state.panelLayoutLocked;
    default: return std::nullopt;
  }
}

std::optional<bool> RenderCommandChecked(MenuBarCommand command, const MenuBarState& state) {
  switch (command) {
    case MenuBarCommand::SetPerfOverlayOff: return state.perfOverlayMode == PerfOverlayMode::Off;
    case MenuBarCommand::SetPerfOverlayFpsPill:
      return state.perfOverlayMode == PerfOverlayMode::FpsPill;
    case MenuBarCommand::SetPerfOverlayFullGraph:
      return state.perfOverlayMode == PerfOverlayMode::FullGraph;
    case MenuBarCommand::SetCompositedRenderingOff:
      return state.compositedRenderingMode == CompositedRenderingMode::Off;
    case MenuBarCommand::SetCompositedRenderingFilterOnly:
      return state.compositedRenderingMode == CompositedRenderingMode::FilterOnly;
    case MenuBarCommand::SetCompositedRenderingOn:
      return state.compositedRenderingMode == CompositedRenderingMode::On;
    default: return std::nullopt;
  }
}

bool CommandChecked(MenuBarCommand command, const MenuBarState& state) {
  if (const std::optional<bool> checked = ViewCommandChecked(command, state)) {
    return *checked;
  }
  if (const std::optional<bool> checked = RenderCommandChecked(command, state)) {
    return *checked;
  }
  return false;
}

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
  if (surface == EditorMenuSurface::Native && SuppressNativeMenuShortcuts(state) &&
      NativeTextCaptureDisables(item.command)) {
    result.enabled = false;
    return result;
  }
  // Canvas-only actions should not mutate a shape behind the source editor.
  // Apply this to both menu surfaces so their enabled states stay in sync.
  if (state.sourcePaneFocused && SourceFocusDisables(item.command)) {
    result.enabled = false;
    return result;
  }
  result.enabled = CommandEnabled(item.command, state);
  result.checked = CommandChecked(item.command, state);
  return result;
}

bool NativeMenuActivationAllowed(const EditorMenuNode& item, const MenuBarState& state,
                                 bool keyEquivalent) {
  return !(keyEquivalent && SuppressNativeMenuShortcuts(state)) &&
         PresentEditorMenuItem(item, state, EditorMenuSurface::Native).enabled;
}

}  // namespace donner::editor
