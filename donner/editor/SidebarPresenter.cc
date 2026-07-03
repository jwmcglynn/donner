#include "donner/editor/SidebarPresenter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/xml/XMLNode.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "embed_resources/BootstrapIcons.h"

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
  const donner::RcString tagName = element.tagName().name;
  const std::string_view tagNameSv = tagName;
  std::string label = "<";
  label.append(tagNameSv.data(), tagNameSv.size());
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

std::string FormatAttributeName(const donner::xml::XMLQualifiedNameRef& name) {
  if (name.namespacePrefix.empty()) {
    return std::string(name.name);
  }

  std::string formatted;
  formatted.reserve(name.namespacePrefix.size() + 1 + name.name.size());
  formatted.append(name.namespacePrefix);
  formatted.push_back(':');
  formatted.append(name.name);
  return formatted;
}

size_t AttributeSortKey(const donner::xml::XMLNode& node, std::string_view source,
                        const donner::xml::XMLQualifiedNameRef& name) {
  if (source.empty()) {
    return std::numeric_limits<size_t>::max();
  }

  if (const auto location = node.getAttributeLocation(source, name); location.has_value()) {
    return location->start.resolveOffset(source).offset.value_or(
        std::numeric_limits<size_t>::max());
  }

  return std::numeric_limits<size_t>::max();
}

template <typename T, donner::svg::PropertyCascade kCascade>
void AppendComputedStyleEntry(std::vector<std::pair<std::string, std::string>>& entries,
                              const donner::svg::Property<T, kCascade>& property) {
  std::ostringstream os;
  if (const auto value = property.get(); value.has_value()) {
    os << *value;
  } else {
    os << "nullopt";
  }
  os << (property.state == donner::svg::PropertyState::NotSet ? " (default)" : " (set)");
  entries.emplace_back(std::string(property.name), os.str());
}

void RenderInspectorSection(const char* heading, const char* tableId,
                            std::span<const std::pair<std::string, std::string>> entries) {
  ImGui::Separator();
  ImGui::TextUnformatted(heading);
  if (entries.empty()) {
    ImGui::TextDisabled("(none)");
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
  if (ImGui::BeginTable(tableId, 2, kFlags)) {
    for (const auto& [name, value] : entries) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(value.c_str());
    }
    ImGui::EndTable();
  }
}

struct PathOperationButton {
  PathOperationKind operation;
  const char* id;
  const char* tooltip;
};

constexpr std::array<PathOperationButton, 4> kPathOperationButtons = {{
    {.operation = PathOperationKind::Union, .id = "##path_operation_union", .tooltip = "Union"},
    {.operation = PathOperationKind::Intersect,
     .id = "##path_operation_intersect",
     .tooltip = "Intersect"},
    {.operation = PathOperationKind::SubtractFront,
     .id = "##path_operation_subtract_front",
     .tooltip = "Subtract Front"},
    {.operation = PathOperationKind::Exclude,
     .id = "##path_operation_exclude",
     .tooltip = "Exclude"},
}};

constexpr float kPathOperationIconSize = 18.0f;
constexpr int kPathOperationIconRasterSizePx = 48;
constexpr ImVec2 kPathOperationButtonFramePadding(6.0f, 4.0f);

std::uint64_t PathOperationIconTextureKey(PathOperationKind operation) {
  constexpr std::uint64_t kIconTextureKeyBase = 0xf600000000000000ull;
  switch (operation) {
    case PathOperationKind::Union: return kIconTextureKeyBase + 1u;
    case PathOperationKind::Intersect: return kIconTextureKeyBase + 2u;
    case PathOperationKind::SubtractFront: return kIconTextureKeyBase + 3u;
    case PathOperationKind::SubtractBack: return kIconTextureKeyBase + 4u;
    case PathOperationKind::Exclude: return kIconTextureKeyBase + 5u;
  }
  return kIconTextureKeyBase;
}

std::span<const unsigned char> BootstrapSvgForPathOperation(PathOperationKind operation) {
  switch (operation) {
    case PathOperationKind::Union: return embedded::kBootstrapUnionSvg;
    case PathOperationKind::Intersect: return embedded::kBootstrapIntersectSvg;
    case PathOperationKind::SubtractFront: return embedded::kBootstrapSubtractSvg;
    case PathOperationKind::SubtractBack: return embedded::kBootstrapSubtractSvg;
    case PathOperationKind::Exclude: return embedded::kBootstrapExcludeSvg;
  }
  return embedded::kBootstrapUnionSvg;
}

const std::optional<svg::RendererBitmap>& CachedPathOperationIconBitmap(
    PathOperationKind operation) {
  switch (operation) {
    case PathOperationKind::Union: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          BootstrapSvgForPathOperation(PathOperationKind::Union), kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::Intersect: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgIcon(BootstrapSvgForPathOperation(PathOperationKind::Intersect),
                                kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::SubtractFront: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgIcon(BootstrapSvgForPathOperation(PathOperationKind::SubtractFront),
                                kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::SubtractBack: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgIcon(BootstrapSvgForPathOperation(PathOperationKind::SubtractBack),
                                kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::Exclude: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          BootstrapSvgForPathOperation(PathOperationKind::Exclude), kPathOperationIconRasterSizePx);
      return bitmap;
    }
  }

  static const std::optional<svg::RendererBitmap> empty;
  return empty;
}

bool RenderPathOperationIconButton(const PathOperationButton& button, bool canApply,
                                   const SidebarPresenter::IconTextureProvider& provider) {
  const ImVec2 iconSize(kPathOperationIconSize, kPathOperationIconSize);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kPathOperationButtonFramePadding);
  const auto popStyle = []() { ImGui::PopStyleVar(); };

  if (provider) {
    const std::optional<svg::RendererBitmap>& bitmap =
        CachedPathOperationIconBitmap(button.operation);
    if (bitmap.has_value()) {
      const SidebarPresenter::IconTexture iconTexture =
          provider(PathOperationIconTextureKey(button.operation), *bitmap);
      if (iconTexture.texture != 0) {
        const ImVec2 uvTopLeft(0.0f, 0.0f);
        const ImVec2 uvBottomRight(static_cast<float>(iconTexture.uvBottomRight.x),
                                   static_cast<float>(iconTexture.uvBottomRight.y));
        const ImVec4 tint = canApply ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                     : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const bool pressed =
            ImGui::ImageButton(button.id, iconTexture.texture, iconSize, uvTopLeft, uvBottomRight,
                               ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tint);
        popStyle();
        return pressed;
      }
    }
  }

  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 buttonSize(iconSize.x + style.FramePadding.x * 2.0f,
                          iconSize.y + style.FramePadding.y * 2.0f);
  const bool pressed = ImGui::InvisibleButton(button.id, buttonSize);
  popStyle();
  return pressed;
}

bool RenderPathOperationsPanel(EditorApp* liveApp,
                               const SidebarPresenter::IconTextureProvider& iconTextureProvider) {
  ImGui::Separator();
  ImGui::TextUnformatted("Path Operations");

  bool queuedMutation = false;
  for (std::size_t i = 0; i < kPathOperationButtons.size(); ++i) {
    const PathOperationButton& button = kPathOperationButtons[i];
    const PathOperationAvailability availability =
        liveApp != nullptr
            ? liveApp->pathOperationAvailability(button.operation)
            : PathOperationAvailability{.canApply = false, .reason = "Rendering in progress"};

    if (i > 0) {
      ImGui::SameLine();
    }

    if (!availability.canApply) {
      ImGui::BeginDisabled();
    }
    if (RenderPathOperationIconButton(button, availability.canApply, iconTextureProvider) &&
        liveApp != nullptr && liveApp->applyPathOperation(button.operation)) {
      queuedMutation = true;
    }
    if (!availability.canApply) {
      ImGui::EndDisabled();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (availability.canApply) {
        ImGui::SetTooltip("%s", button.tooltip);
      } else {
        ImGui::SetTooltip("%s: %s", button.tooltip, availability.reason.c_str());
      }
    }
  }

  return queuedMutation;
}

double FirstSelectedStrokeWidth(const EditorApp& app) {
  const std::vector<svg::SVGElement>& selection = app.selectedElements();
  if (selection.empty()) {
    return 1.0;
  }

  return selection.front().getComputedStyle().strokeWidth.get().value().value;
}

bool RenderStrokeControlsPanel(EditorApp* liveApp) {
  ImGui::Separator();
  ImGui::TextUnformatted("Stroke");

  const bool canEdit = liveApp != nullptr && liveApp->hasSelection();
  float strokeWidth = canEdit ? static_cast<float>(FirstSelectedStrokeWidth(*liveApp)) : 1.0f;

  if (!canEdit) {
    ImGui::BeginDisabled();
  }
  ImGui::SetNextItemWidth(96.0f);
  const bool changed = ImGui::DragFloat("Width", &strokeWidth, 0.1f, 0.0f, 200.0f, "%.2f");
  if (!canEdit) {
    ImGui::EndDisabled();
  }

  if (changed && liveApp != nullptr) {
    liveApp->setActiveStrokeWidth(strokeWidth);
    return liveApp->setStrokeWidthOnSelection(strokeWidth);
  }

  return false;
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

  [[maybe_unused]] const donner::svg::DocumentWriteAccess snapshotWriteAccess =
      app.document().document().writeAccess();
  const auto& selectionList = app.selectedElements();
  TreeNodeSnapshot root;
  captureTreeNode(app.document().document().svgElement(), selectionList, root);
  treeSnapshot_ = std::move(root);

  // Inspector snapshot.
  InspectorSnapshot inspector;
  if (selectionList.size() == 1) {
    inspector.hasSelection = true;
    const donner::svg::SVGElement& selected = selectionList.front();
    const donner::RcString selectedTagName = selected.tagName().name;
    const std::string_view tagSv = selectedTagName;
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

    if (auto xmlNode = donner::xml::XMLNode::TryCast(selected.entityHandle());
        xmlNode.has_value()) {
      auto attributes = xmlNode->attributes();
      std::stable_sort(attributes.begin(), attributes.end(), [&](const auto& lhs, const auto& rhs) {
        return AttributeSortKey(*xmlNode, app.cleanSourceText(), lhs) <
               AttributeSortKey(*xmlNode, app.cleanSourceText(), rhs);
      });
      inspector.xmlAttributes.reserve(attributes.size());
      for (const auto& attributeName : attributes) {
        std::string value;
        if (const auto attributeValue = xmlNode->getAttribute(attributeName);
            attributeValue.has_value()) {
          value.assign(std::string_view(*attributeValue));
        }
        inspector.xmlAttributes.emplace_back(FormatAttributeName(attributeName), std::move(value));
      }
    }

    const auto& computedStyle = selected.getComputedStyle();
    inspector.computedStyle.reserve(9);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.display);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.visibility);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.opacity);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.fill);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.fillOpacity);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.stroke);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.strokeWidth);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.strokeOpacity);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.color);
  }
  inspectorSnapshot_ = std::move(inspector);
}

void SidebarPresenter::renderTreeNode(EditorApp* liveApp, const TreeNodeSnapshot& node,
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

  const donner::Entity entity = node.element->unsafeEntityHandle().entity();
  ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(entity)));
  const bool nodeOpen = ImGui::TreeNodeEx(node.label.c_str(), nodeFlags);

  // Click handling: only valid when we have live access to the EditorApp.
  // When `liveApp` is null (async renderer is busy), clicks are dropped —
  // the selection state freezes along with the rest of the snapshot until
  // the worker finishes and the main loop catches up next frame.
  if (liveApp != nullptr && ImGui::IsItemClicked()) {
    const bool toggleSelection = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
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
      renderTreeNode(liveApp, child, state);
    }
    ImGui::TreePop();
  }

  ImGui::PopID();
}

void SidebarPresenter::renderTreeView(EditorApp* liveApp, TreeViewState& state) const {
  if (!treeSnapshot_.has_value()) {
    ImGui::TextDisabled("(no document)");
    return;
  }
  renderTreeNode(liveApp, *treeSnapshot_, state);
}

bool SidebarPresenter::renderInspector(EditorApp* liveApp, const ViewportState& viewport,
                                       const IconTextureProvider& iconTextureProvider) const {
  bool queuedMutation = false;
  if (!inspectorSnapshot_.hasSelection) {
    if (liveApp != nullptr && liveApp->selectedElements().size() > 1u) {
      ImGui::Text("%zu elements selected", liveApp->selectedElements().size());
      queuedMutation = RenderStrokeControlsPanel(liveApp);
      queuedMutation = RenderPathOperationsPanel(liveApp, iconTextureProvider) || queuedMutation;
    } else {
      ImGui::TextDisabled("Select a single element to inspect attributes.");
      queuedMutation = RenderStrokeControlsPanel(liveApp);
      queuedMutation = RenderPathOperationsPanel(liveApp, iconTextureProvider) || queuedMutation;
    }
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
    queuedMutation = RenderStrokeControlsPanel(liveApp);
    RenderInspectorSection("XML attributes", "##inspector_xml_attributes",
                           inspectorSnapshot_.xmlAttributes);
    RenderInspectorSection("Computed style", "##inspector_computed_style",
                           inspectorSnapshot_.computedStyle);
    queuedMutation = RenderPathOperationsPanel(liveApp, iconTextureProvider) || queuedMutation;
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
  return queuedMutation;
}

}  // namespace donner::editor
