#include "donner/editor/EditorDockLayout.h"

#include <algorithm>

#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {

ImGuiID EditorDockSpaceId() {
  return ImHashStr(kEditorDockSpaceName);
}

ImGuiID EditorCompactDockSpaceId() {
  return ImHashStr(kEditorCompactDockSpaceName);
}

EditorDockNodes BuildDefaultDockLayout(ImGuiID dockspaceId, const EditorDockLayoutParams& params) {
  EditorDockNodes nodes;
  nodes.root = dockspaceId;

  // Start from a clean root node sized to the host so the split ratios below
  // resolve against real pixels.
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, params.size);

  if (!params.includeSidebars) {
    nodes.central = dockspaceId;
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetNode(dockspaceId)) {
      centralNode->SetLocalFlags(centralNode->LocalFlags | ImGuiDockNodeFlags_CentralNode |
                                 ImGuiDockNodeFlags_NoTabBar);
    }
    ImGui::DockBuilderDockWindow(kRenderPaneWindowName, dockspaceId);
    ImGui::DockBuilderFinish(dockspaceId);
    return nodes;
  }

  // Split the right panel column off the canvas. The opposite side is the
  // central node that hosts the canvas / render pane.
  ImGuiID rightColumnId = 0;
  ImGuiID centralId = 0;
  ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right, params.rightColumnRatio, &rightColumnId,
                              &centralId);
  nodes.central = centralId;

  // Flag the canvas node as the dockspace's central node with no tab bar, so it
  // reads as a plain viewport and is reported by DockBuilderGetCentralNode().
  if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetNode(centralId)) {
    centralNode->SetLocalFlags(centralNode->LocalFlags | ImGuiDockNodeFlags_CentralNode |
                               ImGuiDockNodeFlags_NoTabBar);
  }

  // Stack Layers over the rest of the right column.
  ImGuiID layersId = 0;
  ImGuiID rightLowerId = 0;
  ImGui::DockBuilderSplitNode(rightColumnId, ImGuiDir_Up, params.layersRatio, &layersId,
                              &rightLowerId);
  nodes.rightTop = layersId;

  // The right column below Layers hosts the Inspector, with the Compositor
  // Debug Info panel docked at the bottom when it is enabled. Splitting the
  // inspector area this way keeps the compositor diagnostics reachable in the
  // side column while leaving its pane resizable via the splitter.
  if (params.includeCompositorDebug) {
    ImGuiID compositorId = 0;
    ImGuiID inspectorId = 0;
    ImGui::DockBuilderSplitNode(rightLowerId, ImGuiDir_Down, params.compositorRatio, &compositorId,
                                &inspectorId);
    nodes.rightMid = inspectorId;
    nodes.rightBottom = compositorId;
    ImGui::DockBuilderDockWindow(kCompositorDebugWindowName, compositorId);
  } else {
    nodes.rightMid = rightLowerId;
    nodes.rightBottom = 0;
  }

  ImGui::DockBuilderDockWindow(kRenderPaneWindowName, centralId);
  ImGui::DockBuilderDockWindow(kLayersWindowName, layersId);
  ImGui::DockBuilderDockWindow(kInspectorWindowName, nodes.rightMid);

  ImGui::DockBuilderFinish(dockspaceId);
  return nodes;
}

namespace {

ImGuiDockNode* DockNodeForWindow(const char* name) {
  if (const ImGuiWindow* window = ImGui::FindWindowByName(name)) {
    if (window->DockId != 0) {
      return ImGui::DockBuilderGetNode(window->DockId);
    }
  }
  const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(name));
  return settings != nullptr ? ImGui::DockBuilderGetNode(settings->DockId) : nullptr;
}

bool BelongsToDockspace(const ImGuiDockNode* node, ImGuiID dockspaceId) {
  if (node == nullptr) {
    return false;
  }
  while (node->ParentNode != nullptr) {
    node = node->ParentNode;
  }
  return node->ID == dockspaceId;
}

}  // namespace

void EnsureAnimationModeDock(ImGuiID dockspaceId, float height) {
  ImGuiDockNode* existingHeader = DockNodeForWindow(kAnimationModeWindowName);
  if (BelongsToDockspace(existingHeader, dockspaceId)) {
    ImGui::DockBuilderSetNodeSize(existingHeader->ID, ImVec2(existingHeader->Size.x, height));
    existingHeader->WantLockSizeOnce = true;
    return;
  }
  const ImGuiDockNode* layers = DockNodeForWindow(kLayersWindowName);
  if (!BelongsToDockspace(layers, dockspaceId) || layers->Size.y <= 0.0f) {
    return;
  }
  ImGuiID headerId = 0;
  ImGuiID layersId = 0;
  ImGui::DockBuilderSplitNode(layers->ID, ImGuiDir_Up,
                              std::clamp(height / layers->Size.y, 0.05f, 0.6f), &headerId,
                              &layersId);
  ImGui::DockBuilderDockWindow(kAnimationModeWindowName, headerId);
  ImGui::DockBuilderDockWindow(kLayersWindowName, layersId);
  if (ImGuiDockNode* header = ImGui::DockBuilderGetNode(headerId)) {
    header->SetLocalFlags(header->LocalFlags | ImGuiDockNodeFlags_NoTabBar |
                          ImGuiDockNodeFlags_NoResizeY | ImGuiDockNodeFlags_NoDocking);
  }
  ImGui::DockBuilderFinish(dockspaceId);
}

ImGuiDockNodeFlags EditorDockSpaceFlags(bool locked) {
  // The canvas central node always stays clear of docked panels.
  ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_NoDockingOverCentralNode;
  if (locked) {
    // Fix the layout structure: block new splits and undocking (tear-off) so
    // panels cannot be accidentally rearranged. The existing splitters stay
    // resizable so the right column width and panel heights (e.g. the
    // Layers/Inspector pane) can be adjusted.
    flags |= ImGuiDockNodeFlags_NoDockingSplit | ImGuiDockNodeFlags_NoUndocking;
  }
  return flags;
}

}  // namespace donner::editor
