#include "donner/editor/EditorDockLayout.h"

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

ImGuiDockNodeFlags EditorDockSpaceFlags(bool locked) {
  // The canvas central node always stays clear of docked panels.
  ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_NoDockingOverCentralNode;
  if (locked) {
    // Disable the splitter/resizer, prevent new splits, and block undocking
    // (tear-off) so the locked layout cannot be accidentally rearranged.
    flags |= ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoDockingSplit |
             ImGuiDockNodeFlags_NoUndocking;
  }
  return flags;
}

}  // namespace donner::editor
