#pragma once
/// @file

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

/// Window titles for the dockable editor panels. Each string is the key
/// DockBuilder binds to a node, so it must match the exact string passed to
/// `ImGui::Begin()` when the panel is drawn.
inline constexpr const char* kRenderPaneWindowName = "Render";
inline constexpr const char* kLayersWindowName = "Layers";
inline constexpr const char* kInspectorWindowName = "Inspector";
inline constexpr const char* kCompositorDebugWindowName = "Compositor Debug";

/// Stable string hashed into the editor's root DockSpace id.
inline constexpr const char* kEditorDockSpaceName = "EditorDockSpace";

/// Node ids produced by \ref BuildDefaultDockLayout. Ids of zero mean the node
/// was not created (e.g. \ref EditorDockNodes::rightBottom when the compositor
/// debug panel is excluded from the layout).
struct EditorDockNodes {
  /// Root DockSpace node id (equal to the id passed to \ref BuildDefaultDockLayout).
  ImGuiID root = 0;
  /// Central node hosting the canvas / render pane.
  ImGuiID central = 0;
  /// Top-right node hosting the Layers panel.
  ImGuiID rightTop = 0;
  /// Middle-right node hosting the Inspector panel.
  ImGuiID rightMid = 0;
  /// Bottom-right node hosting the Compositor Debug panel (zero when excluded).
  ImGuiID rightBottom = 0;
};

/// Inputs controlling the default editor dock layout proportions.
struct EditorDockLayoutParams {
  /// Root DockSpace size in pixels.
  ImVec2 size = ImVec2(0.0f, 0.0f);
  /// Fraction of the width assigned to the right panel column.
  float rightColumnRatio = 0.26f;
  /// Fraction of the right column height assigned to the Layers panel (top).
  float layersRatio = 0.34f;
  /// Fraction of the remaining right column height assigned to the Compositor
  /// Debug panel (bottom). Ignored when \ref includeCompositorDebug is false.
  float compositorRatio = 0.34f;
  /// Whether the Compositor Debug panel gets a reserved node in the layout.
  bool includeCompositorDebug = false;
};

/**
 * Build the editor's default panel layout under `dockspaceId`, discarding any
 * previously built nodes. The central node hosts the canvas; the right column
 * stacks Layers over Inspector (and Compositor Debug at the bottom when
 * requested). The canvas node is flagged as the central node with no tab bar so
 * it reads as a plain viewport rather than a docked tab.
 *
 * Requires a live ImGui context. The bound windows do not need to have been
 * submitted yet; DockBuilder records the binding for their next `Begin()`.
 *
 * @param dockspaceId Root DockSpace id (see \ref kEditorDockSpaceName).
 * @param params Layout proportions and panel inclusion.
 * @return Ids of the created nodes.
 */
EditorDockNodes BuildDefaultDockLayout(ImGuiID dockspaceId, const EditorDockLayoutParams& params);

/**
 * Shared dock-node flags to pass to `ImGui::DockSpace()` for the given lock
 * state. When locked, splitters, resizing, and undocking (tear-off) are all
 * disabled so the layout cannot be accidentally torn apart; when unlocked the
 * layout is freely rearrangeable within the main viewport (multi-viewport
 * tear-off to OS windows stays disabled at the config-flag level).
 *
 * @param locked Whether the layout is locked.
 */
[[nodiscard]] ImGuiDockNodeFlags EditorDockSpaceFlags(bool locked);

}  // namespace donner::editor
