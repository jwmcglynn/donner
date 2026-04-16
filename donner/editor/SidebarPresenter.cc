#include "donner/editor/SidebarPresenter.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "imgui.h"

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

bool RenderTreeRecursive(EditorApp& app, const donner::svg::SVGElement& element,
                         TreeViewState& state) {
  const bool hasChildren = element.firstChild().has_value();
  const bool onSelectionPath = state.pendingScroll && state.scrollTarget.has_value() &&
                               IsAncestorOrSelf(element, *state.scrollTarget);
  if (hasChildren && onSelectionPath) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  }

  ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_SpanAvailWidth;
  if (!hasChildren) {
    nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }
  if (IsSelectedInTree(std::span<const donner::svg::SVGElement>(app.selectedElements()), element)) {
    nodeFlags |= ImGuiTreeNodeFlags_Selected;
  }

  const donner::Entity entity = element.entityHandle().entity();
  ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(entity)));
  const std::string label = BuildTreeNodeLabel(element);
  const bool nodeOpen = ImGui::TreeNodeEx(label.c_str(), nodeFlags);

  if (ImGui::IsItemClicked()) {
    const bool toggleSelection = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    if (toggleSelection) {
      app.toggleInSelection(element);
    } else {
      app.setSelection(element);
    }
    state.selectionChangedInTree = true;
    state.pendingScroll = false;
  }

  if (state.pendingScroll && state.scrollTarget.has_value() && *state.scrollTarget == element) {
    ImGui::SetScrollHereY();
    state.pendingScroll = false;
  }

  if (nodeOpen && hasChildren) {
    for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
      RenderTreeRecursive(app, *child, state);
    }
    ImGui::TreePop();
  }

  ImGui::PopID();
  return state.selectionChangedInTree;
}

void RenderInspectorInternal(const EditorApp& app) {
  if (!app.hasSelection()) {
    ImGui::TextDisabled("Nothing selected. Click an element to inspect.");
    return;
  }

  const donner::svg::SVGElement& selected = *app.selectedElement();
  const std::string_view tagSv = selected.tagName().name;
  const donner::RcString idStr = selected.id();
  const std::string_view idSv = idStr;
  if (!idSv.empty()) {
    ImGui::Text("Selected: <%.*s id=\"%.*s\">", static_cast<int>(tagSv.size()), tagSv.data(),
                static_cast<int>(idSv.size()), idSv.data());
  } else {
    ImGui::Text("Selected: <%.*s>", static_cast<int>(tagSv.size()), tagSv.data());
  }

  if (selected.isa<donner::svg::SVGGeometryElement>()) {
    if (auto bounds = selected.cast<donner::svg::SVGGeometryElement>().worldBounds();
        bounds.has_value()) {
      ImGui::Text("Bounds: (%.1f, %.1f) %.1f × %.1f", bounds->topLeft.x, bounds->topLeft.y,
                  bounds->width(), bounds->height());
    }
  }

  if (selected.isa<donner::svg::SVGGraphicsElement>()) {
    const donner::Transform2d xform = selected.cast<donner::svg::SVGGraphicsElement>().transform();
    ImGui::Text("Transform: [%.3f %.3f %.3f %.3f  %.2f %.2f]", xform.data[0], xform.data[1],
                xform.data[2], xform.data[3], xform.data[4], xform.data[5]);
  }
}

}  // namespace

void SidebarPresenter::renderTreeView(EditorApp& app, TreeViewState& state) const {
  if (!app.hasDocument()) {
    ImGui::TextDisabled("(no document)");
    return;
  }

  RenderTreeRecursive(app, app.document().document().svgElement(), state);
}

void SidebarPresenter::renderInspector(const EditorApp& app, const ViewportState& viewport) const {
  RenderInspectorInternal(app);
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
