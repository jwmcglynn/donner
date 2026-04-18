#include "donner/editor/SidebarPresenter.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectTool.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

namespace {

bool IsAncestorOrSelf(const donner::svg::SVGElement& ancestor,
                      const donner::svg::SVGElement& node) {
  for (std::optional<donner::svg::SVGElement> current = node; current.has_value();
       current = current->parentElement()) {
    if (*current == ancestor) {
      return true;
    }
  }
  return false;
}

bool IsSelectedInTree(std::span<const donner::svg::SVGElement> selection,
                      const donner::svg::SVGElement& element) {
  return std::find(selection.begin(), selection.end(), element) != selection.end();
}

std::string BuildTreeNodeLabel(const donner::svg::SVGElement& element) {
  const std::string_view tagName = element.tagName().name;
  std::string label = "<";
  label.append(tagName.data(), tagName.size());
  label.push_back('>');

  const donner::RcString id = element.id();
  const std::string_view idSv = id;
  if (!idSv.empty()) {
    label.push_back(' ');
    label.push_back('#');
    label.append(idSv.data(), idSv.size());
  }

  return label;
}

}  // namespace

void SidebarPresenter::captureTreeNode(const donner::svg::SVGElement& element,
                                        std::span<const donner::svg::SVGElement> selection,
                                        TreeNodeSnapshot& out) {
  out.element = element;
  out.label = BuildTreeNodeLabel(element);
  out.isSelected = IsSelectedInTree(selection, element);
  out.children.clear();
  for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
    out.children.emplace_back();
    captureTreeNode(*child, selection, out.children.back());
  }
}

void SidebarPresenter::refreshSnapshot(const EditorApp& app) {
  if (!app.hasDocument()) {
    treeSnapshot_.reset();
    inspectorSnapshot_ = InspectorSnapshot{};
    return;
  }

  const auto& selectionList = app.selectedElements();
  TreeNodeSnapshot root;
  captureTreeNode(app.document().document().svgElement(), selectionList, root);
  treeSnapshot_ = std::move(root);

  // Inspector snapshot.
  InspectorSnapshot inspector;
  if (app.hasSelection()) {
    inspector.hasSelection = true;
    const donner::svg::SVGElement& selected = *app.selectedElement();
    const std::string_view tagSv = selected.tagName().name;
    const donner::RcString idStr = selected.id();
    const std::string_view idSv = idStr;
    if (!idSv.empty()) {
      inspector.titleText = "Selected: <";
      inspector.titleText.append(tagSv.data(), tagSv.size());
      inspector.titleText += " id=\"";
      inspector.titleText.append(idSv.data(), idSv.size());
      inspector.titleText += "\">";
    } else {
      inspector.titleText = "Selected: <";
      inspector.titleText.append(tagSv.data(), tagSv.size());
      inspector.titleText += ">";
    }

    if (selected.isa<donner::svg::SVGGeometryElement>()) {
      inspector.bounds = selected.cast<donner::svg::SVGGeometryElement>().worldBounds();
    }
    if (selected.isa<donner::svg::SVGGraphicsElement>()) {
      inspector.transform = selected.cast<donner::svg::SVGGraphicsElement>().transform();
    }
  }
  inspectorSnapshot_ = std::move(inspector);
}

void SidebarPresenter::renderTreeNode(EditorApp* liveApp, SelectTool* selectTool,
                                       const TreeNodeSnapshot& node,
                                       TreeViewState& state) const {
  const bool hasChildren = !node.children.empty();
  const bool onSelectionPath = state.pendingScroll && state.scrollTarget.has_value() &&
                               IsAncestorOrSelf(*node.element, *state.scrollTarget);
  if (hasChildren && onSelectionPath) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  }

  ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_SpanAvailWidth;
  if (!hasChildren) {
    nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }
  if (node.isSelected) {
    nodeFlags |= ImGuiTreeNodeFlags_Selected;
  }

  const donner::Entity entity = node.element->entityHandle().entity();
  ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(entity)));
  const bool nodeOpen = ImGui::TreeNodeEx(node.label.c_str(), nodeFlags);

  // Click handling: only valid when we have live access to the EditorApp.
  // When `liveApp` is null (async renderer is busy), clicks are dropped —
  // the selection state freezes along with the rest of the snapshot until
  // the worker finishes and the main loop catches up next frame.
  if (liveApp != nullptr && ImGui::IsItemClicked()) {
    const bool toggleSelection = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    // Flush any deferred drag mutation before the selection changes.
    // Without this, the fresh render that the selection change is about
    // to kick off would still see the pre-drag DOM transform for the
    // last-dragged entity and snap it back to the pre-drag position.
    if (selectTool != nullptr) {
      selectTool->commitPendingDragMutation(*liveApp);
    }
    if (toggleSelection) {
      liveApp->toggleInSelection(*node.element);
    } else {
      liveApp->setSelection(*node.element);
    }
    state.selectionChangedInTree = true;
    state.pendingScroll = false;
  }

  if (nodeOpen && hasChildren) {
    for (const auto& child : node.children) {
      renderTreeNode(liveApp, selectTool, child, state);
    }
    ImGui::TreePop();
  }

  ImGui::PopID();
}

void SidebarPresenter::renderTreeView(EditorApp* liveApp, SelectTool* selectTool,
                                       TreeViewState& state) const {
  if (!treeSnapshot_.has_value()) {
    ImGui::TextDisabled("(no document)");
    return;
  }
  renderTreeNode(liveApp, selectTool, *treeSnapshot_, state);
}

void SidebarPresenter::renderInspector(const ViewportState& viewport) const {
  if (!inspectorSnapshot_.hasSelection) {
    ImGui::TextDisabled("Nothing selected. Click an element to inspect.");
  } else {
    ImGui::TextUnformatted(inspectorSnapshot_.titleText.c_str());
    if (inspectorSnapshot_.bounds.has_value()) {
      const auto& b = *inspectorSnapshot_.bounds;
      ImGui::Text("Bounds: (%.1f, %.1f) %.1f × %.1f", b.topLeft.x, b.topLeft.y, b.width(),
                  b.height());
    }
    if (inspectorSnapshot_.transform.has_value()) {
      const auto& xform = *inspectorSnapshot_.transform;
      ImGui::Text("Transform: [%.3f %.3f %.3f %.3f  %.2f %.2f]", xform.data[0], xform.data[1],
                  xform.data[2], xform.data[3], xform.data[4], xform.data[5]);
    }
  }
  ImGui::Separator();
  ImGui::Text("Zoom: %.0f%%", viewport.zoom * 100.0);
  ImGui::Text("Pan anchor: doc=(%.1f, %.1f) screen=(%.0f, %.0f)", viewport.panDocPoint.x,
              viewport.panDocPoint.y, viewport.panScreenPoint.x, viewport.panScreenPoint.y);
  ImGui::Text("DPR: %.2fx", viewport.devicePixelRatio);
  ImGui::TextDisabled("scroll = pan");
  ImGui::TextDisabled("Cmd+scroll = zoom");
  ImGui::TextDisabled("space+drag = pan");
  ImGui::TextDisabled("Cmd+0 = 100%%");
}

}  // namespace donner::editor
