#include "donner/editor/SidebarPresenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/MathUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LockState.h"
#include "donner/editor/UndoTimeline.h"
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

/// Shared width for the inspector's editable numeric fields (stroke width,
/// transform scalars, raw matrix cells) so every DragFloat lines up.
constexpr float kInspectorFieldWidth = 96.0f;

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
  ImGui::SetNextItemWidth(kInspectorFieldWidth);
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

/// Below this document-space span (in user units), a bounds axis is treated
/// as degenerate and the matching Width / Height field is disabled - a scale
/// factor against a ~zero span would explode.
constexpr double kMinimumSpanForScale = 1e-6;

}  // namespace

std::optional<DecomposedTransform> DecomposeTransform(const Transform2d& transform) {
  const double a = transform.data[0];
  const double b = transform.data[1];
  const double c = transform.data[2];
  const double d = transform.data[3];

  // Length of the x basis column. A ~zero column means the matrix collapses
  // the x axis entirely; there is no meaningful rotation to extract.
  const double scaleX = std::hypot(a, b);
  constexpr double kSingularEpsilon = 1e-12;
  if (!(scaleX > kSingularEpsilon) || !std::isfinite(scaleX)) {
    return std::nullopt;
  }

  // The matrix is scale-rotate-translate exactly when its basis columns are
  // orthogonal; a non-zero (relative) dot product means skew.
  const double columnDot = a * c + b * d;
  const double columnYNorm = std::hypot(c, d);
  constexpr double kSkewTolerance = 1e-6;
  if (columnYNorm > 0.0 && std::abs(columnDot) > kSkewTolerance * scaleX * columnYNorm) {
    return std::nullopt;
  }

  DecomposedTransform result;
  result.translation = Vector2d(transform.data[4], transform.data[5]);
  result.rotationRadians = std::atan2(b, a);
  // Signed so flips (negative determinant) stay representable.
  result.scale = Vector2d(scaleX, (a * d - b * c) / scaleX);
  return result;
}

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
    // The document is gone; the edit's element handle and baseline are
    // meaningless now, so drop the in-progress edit instead of committing.
    transformEdit_.reset();
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
  // When `liveApp` is null (async renderer is busy), clicks are dropped -
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
                                       const IconTextureProvider& iconTextureProvider) {
  bool queuedMutation = false;
  // Selection left the single-element inspector while a transform edit was
  // still pending (e.g. its commit was deferred past a busy frame); land the
  // undo entry now instead of holding it indefinitely.
  if (transformEdit_.has_value() && liveApp != nullptr && !inspectorSnapshot_.hasSelection) {
    commitTransformEdit(*liveApp);
  }
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
    queuedMutation = renderTransformPanel(liveApp);
    queuedMutation = RenderStrokeControlsPanel(liveApp) || queuedMutation;
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

bool SidebarPresenter::renderTransformPanel(EditorApp* liveApp) {
  if (!inspectorSnapshot_.transform.has_value()) {
    return false;
  }

  bool queuedMutation = false;

  // The single selected element, when the app is live this frame. All edits
  // target this element; when `liveApp` is null (async renderer busy) the
  // fields render disabled from the snapshot, mirroring tree-click gating.
  std::optional<svg::SVGElement> liveElement;
  if (liveApp != nullptr && liveApp->selectedElements().size() == 1u) {
    liveElement = liveApp->selectedElements().front();
  }

  // An in-progress edit whose element no longer matches the live selection
  // can't keep composing against its baseline; commit what was applied so
  // the undo entry isn't lost.
  if (transformEdit_.has_value() && liveApp != nullptr &&
      (!liveElement.has_value() || *liveElement != transformEdit_->element)) {
    commitTransformEdit(*liveApp);
  }

  // Finalize edits that deactivated on a frame without live app access, or
  // whose item ImGui dropped while the widgets rendered disabled (busy
  // renderer mid-drag). `IsAnyItemActive` is false in both cases.
  if (transformEdit_.has_value() && (transformEdit_->pendingCommit || !ImGui::IsAnyItemActive())) {
    if (liveApp != nullptr) {
      commitTransformEdit(*liveApp);
    } else {
      transformEdit_->pendingCommit = true;
    }
  }

  const Transform2d& snapshotTransform = *inspectorSnapshot_.transform;
  const std::optional<DecomposedTransform> snapshotDecomposed =
      DecomposeTransform(snapshotTransform);
  const std::optional<Box2d>& bounds = inspectorSnapshot_.bounds;

  const bool liveEditable = liveElement.has_value() && !IsLocked(*liveElement);
  const bool decomposable = snapshotDecomposed.has_value();

  ImGui::Separator();
  ImGui::TextUnformatted("Transform");

  const bool canEditPosition = liveEditable && decomposable && bounds.has_value();
  const bool canEditWidth =
      canEditPosition && static_cast<double>(bounds->width()) > kMinimumSpanForScale;
  const bool canEditHeight =
      canEditPosition && static_cast<double>(bounds->height()) > kMinimumSpanForScale;
  const bool canEditRotation = liveEditable && decomposable;

  const float xValue = bounds.has_value() ? static_cast<float>(bounds->topLeft.x) : 0.0f;
  const float yValue = bounds.has_value() ? static_cast<float>(bounds->topLeft.y) : 0.0f;
  const float widthValue = bounds.has_value() ? static_cast<float>(bounds->width()) : 0.0f;
  const float heightValue = bounds.has_value() ? static_cast<float>(bounds->height()) : 0.0f;
  const float rotationValue = decomposable
                                  ? static_cast<float>(snapshotDecomposed->rotationRadians *
                                                       MathConstants<double>::kRadToDeg)
                                  : 0.0f;

  queuedMutation =
      renderTransformFieldDrag(liveApp, TransformField::PositionX, "X##transform_x", xValue,
                               canEditPosition, "Move element", 1.0f, "%.2f") ||
      queuedMutation;
  ImGui::SameLine();
  queuedMutation =
      renderTransformFieldDrag(liveApp, TransformField::PositionY, "Y##transform_y", yValue,
                               canEditPosition, "Move element", 1.0f, "%.2f") ||
      queuedMutation;
  queuedMutation =
      renderTransformFieldDrag(liveApp, TransformField::Width, "W##transform_w", widthValue,
                               canEditWidth, "Resize element", 1.0f, "%.2f") ||
      queuedMutation;
  ImGui::SameLine();
  queuedMutation =
      renderTransformFieldDrag(liveApp, TransformField::Height, "H##transform_h", heightValue,
                               canEditHeight, "Resize element", 1.0f, "%.2f") ||
      queuedMutation;
  queuedMutation =
      renderTransformFieldDrag(liveApp, TransformField::Rotation, "Rotation##transform_r",
                               rotationValue, canEditRotation, "Rotate element", 0.5f, "%.1f") ||
      queuedMutation;
  if (!decomposable) {
    ImGui::TextDisabled("Matrix has skew; edit the raw values below.");
  }

  // Raw matrix disclosure: collapsed by default, always editable so skewed
  // and otherwise non-decomposable matrices keep an editing route.
  if (ImGui::TreeNode("Matrix##transform_matrix")) {
    static constexpr std::array<const char*, 6> kMatrixLabels = {
        "a##transform_mat0", "b##transform_mat1", "c##transform_mat2",
        "d##transform_mat3", "e##transform_mat4", "f##transform_mat5"};
    for (int i = 0; i < 6; ++i) {
      const bool editingThisCell =
          transformEdit_.has_value() && transformEdit_->field == TransformField::Matrix &&
          transformEdit_->matrixIndex == i && !transformEdit_->pendingCommit;
      double value = editingThisCell ? transformEdit_->matrixValues[i] : snapshotTransform.data[i];

      if ((i % 2) == 1) {
        ImGui::SameLine();
      }
      ImGui::SetNextItemWidth(kInspectorFieldWidth);
      if (!liveEditable) {
        ImGui::BeginDisabled();
      }
      const bool changed = ImGui::DragScalar(kMatrixLabels[i], ImGuiDataType_Double, &value, 0.01f,
                                             nullptr, nullptr, "%.6g");
      if (!liveEditable) {
        ImGui::EndDisabled();
      }

      if (liveApp == nullptr) {
        continue;
      }
      if (liveEditable && ImGui::IsItemActivated()) {
        beginTransformEdit(*liveApp, TransformField::Matrix, i, "Edit transform");
      }
      if (transformEdit_.has_value() && transformEdit_->field == TransformField::Matrix &&
          transformEdit_->matrixIndex == i && !transformEdit_->pendingCommit) {
        if (changed && std::isfinite(value)) {
          transformEdit_->matrixValues[i] = value;
          transformEdit_->fieldValue = value;
          queuedMutation = applyTransformEdit(*liveApp, value) || queuedMutation;
        }
        if (ImGui::IsItemDeactivated()) {
          commitTransformEdit(*liveApp);
        }
      }
    }
    ImGui::TreePop();
  }

  return queuedMutation;
}

bool SidebarPresenter::renderTransformFieldDrag(EditorApp* liveApp, TransformField field,
                                                const char* label, float displayValue, bool canEdit,
                                                const char* undoLabel, float dragSpeed,
                                                const char* format) {
  const bool editingThisField = transformEdit_.has_value() && transformEdit_->field == field &&
                                !transformEdit_->pendingCommit;
  float value = editingThisField ? static_cast<float>(transformEdit_->fieldValue) : displayValue;

  ImGui::SetNextItemWidth(kInspectorFieldWidth);
  if (!canEdit) {
    ImGui::BeginDisabled();
  }
  const bool changed = ImGui::DragFloat(label, &value, dragSpeed, 0.0f, 0.0f, format);
  if (!canEdit) {
    ImGui::EndDisabled();
  }

  if (liveApp == nullptr) {
    return false;
  }

  bool queuedMutation = false;
  if (canEdit && ImGui::IsItemActivated()) {
    beginTransformEdit(*liveApp, field, /*matrixIndex=*/0, undoLabel);
  }
  if (transformEdit_.has_value() && transformEdit_->field == field &&
      !transformEdit_->pendingCommit) {
    if (changed && std::isfinite(value)) {
      transformEdit_->fieldValue = static_cast<double>(value);
      queuedMutation = applyTransformEdit(*liveApp, static_cast<double>(value));
    }
    if (ImGui::IsItemDeactivated()) {
      commitTransformEdit(*liveApp);
    }
  }
  return queuedMutation;
}

void SidebarPresenter::beginTransformEdit(EditorApp& liveApp, TransformField field, int matrixIndex,
                                          const char* undoLabel) {
  // A previous edit still holding state (e.g. its commit was deferred) must
  // land its undo entry before the slot is reused.
  if (transformEdit_.has_value()) {
    commitTransformEdit(liveApp);
  }
  if (liveApp.selectedElements().size() != 1u) {
    return;
  }
  const svg::SVGElement element = liveApp.selectedElements().front();

  // §concurrent-dom: raw ECS reads from the UI thread need a scoped read
  // access while the document runs in ThreadingMode::ConcurrentDom.
  const bool isGraphics =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        return element.isa<svg::SVGGraphicsElement>();
      });
  if (!isGraphics) {
    return;
  }

  TransformEditState state{
      .element = element, .field = field, .matrixIndex = matrixIndex, .undoLabel = undoLabel};
  element.withReadAccess([&element, &state](svg::DocumentReadAccess&, EntityHandle) {
    state.startTransform = element.cast<svg::SVGGraphicsElement>().transform();
    if (element.isa<svg::SVGGeometryElement>()) {
      state.startBounds = element.cast<svg::SVGGeometryElement>().worldBounds();
    }
    // Verbatim attribute bytes so undo restores the user's original source
    // text instead of the canonical serializer output.
    state.sourceTransformAttributeValue = element.getAttribute("transform");
  });
  state.currentTransform = state.startTransform;
  state.startDecomposed = DecomposeTransform(state.startTransform);
  // Captured now, while the source is still in sync with the DOM, so undo
  // and the source writeback resolve the element after document changes.
  state.writebackTarget = captureAttributeWritebackTarget(element);
  for (int i = 0; i < 6; ++i) {
    state.matrixValues[static_cast<std::size_t>(i)] = state.startTransform.data[i];
  }

  switch (field) {
    case TransformField::PositionX:
      state.fieldValue = state.startBounds.has_value() ? state.startBounds->topLeft.x : 0.0;
      break;
    case TransformField::PositionY:
      state.fieldValue = state.startBounds.has_value() ? state.startBounds->topLeft.y : 0.0;
      break;
    case TransformField::Width:
      state.fieldValue = state.startBounds.has_value() ? state.startBounds->width() : 0.0;
      break;
    case TransformField::Height:
      state.fieldValue = state.startBounds.has_value() ? state.startBounds->height() : 0.0;
      break;
    case TransformField::Rotation:
      state.fieldValue =
          state.startDecomposed.has_value()
              ? state.startDecomposed->rotationRadians * MathConstants<double>::kRadToDeg
              : 0.0;
      break;
    case TransformField::Matrix:
      state.fieldValue = state.matrixValues[static_cast<std::size_t>(matrixIndex)];
      break;
  }

  transformEdit_ = std::move(state);
}

Transform2d SidebarPresenter::composeFieldTransform(const TransformEditState& state,
                                                    double value) const {
  // Deltas compose in document space and post-multiply onto the transform
  // captured at activation, matching SelectTool's move/resize/rotate math
  // (Donner transforms apply left-to-right, so `start * delta` applies the
  // element's own transform first).
  switch (state.field) {
    case TransformField::PositionX: {
      if (!state.startBounds.has_value()) {
        return state.currentTransform;
      }
      return state.startTransform *
             Transform2d::Translate(value - state.startBounds->topLeft.x, 0.0);
    }
    case TransformField::PositionY: {
      if (!state.startBounds.has_value()) {
        return state.currentTransform;
      }
      return state.startTransform *
             Transform2d::Translate(0.0, value - state.startBounds->topLeft.y);
    }
    case TransformField::Width: {
      if (!state.startBounds.has_value() ||
          !(static_cast<double>(state.startBounds->width()) > kMinimumSpanForScale) ||
          !(value > kMinimumSpanForScale)) {
        return state.currentTransform;
      }
      const Box2d& box = *state.startBounds;
      const double factor = value / box.width();
      return state.startTransform * Transform2d::Translate(-box.topLeft) *
             Transform2d::Scale(factor, 1.0) * Transform2d::Translate(box.topLeft);
    }
    case TransformField::Height: {
      if (!state.startBounds.has_value() ||
          !(static_cast<double>(state.startBounds->height()) > kMinimumSpanForScale) ||
          !(value > kMinimumSpanForScale)) {
        return state.currentTransform;
      }
      const Box2d& box = *state.startBounds;
      const double factor = value / box.height();
      return state.startTransform * Transform2d::Translate(-box.topLeft) *
             Transform2d::Scale(1.0, factor) * Transform2d::Translate(box.topLeft);
    }
    case TransformField::Rotation: {
      if (!state.startDecomposed.has_value()) {
        return state.currentTransform;
      }
      const double deltaRadians =
          value * MathConstants<double>::kDegToRad - state.startDecomposed->rotationRadians;
      const Vector2d center =
          state.startBounds.has_value()
              ? (state.startBounds->topLeft + state.startBounds->bottomRight) * 0.5
              : state.startTransform.translation();
      return state.startTransform * Transform2d::Translate(-center) *
             Transform2d::Rotate(deltaRadians) * Transform2d::Translate(center);
    }
    case TransformField::Matrix: {
      Transform2d result(Transform2d::uninitialized);
      for (int i = 0; i < 6; ++i) {
        result.data[i] = state.matrixValues[static_cast<std::size_t>(i)];
      }
      return result;
    }
  }
  return state.currentTransform;
}

bool SidebarPresenter::applyTransformEdit(EditorApp& liveApp, double value) {
  if (!transformEdit_.has_value() || !std::isfinite(value)) {
    return false;
  }
  const Transform2d newTransform = composeFieldTransform(*transformEdit_, value);
  // Degenerate inputs (e.g. a typed-in zero width) compose to the unchanged
  // current transform; skip the write so a no-op edit doesn't queue commands
  // or record an undo entry claiming a change.
  if (std::equal(std::begin(newTransform.data), std::end(newTransform.data),
                 std::begin(transformEdit_->currentTransform.data))) {
    return false;
  }
  transformEdit_->currentTransform = newTransform;
  transformEdit_->changed = true;
  liveApp.applyMutation(EditorCommand::SetTransformCommand(transformEdit_->element, newTransform));
  return true;
}

void SidebarPresenter::commitTransformEdit(EditorApp& liveApp) {
  if (!transformEdit_.has_value()) {
    return;
  }
  TransformEditState state = std::move(*transformEdit_);
  transformEdit_.reset();
  if (!state.changed) {
    return;
  }

  // One undo step per completed edit, mirroring SelectTool's end-of-drag
  // recording: per-frame SetTransformCommands already moved the DOM, so only
  // the snapshot pair and the source writeback remain.
  UndoSnapshot before{.element = state.element,
                      .transform = state.startTransform,
                      .writebackTarget = state.writebackTarget,
                      .sourceTransformAttributeValue = state.sourceTransformAttributeValue,
                      .restoreSourceTransformAttributeValue = true};
  UndoSnapshot after{.element = state.element,
                     .transform = state.currentTransform,
                     .writebackTarget = state.writebackTarget};
  liveApp.undoTimeline().record(state.undoLabel, std::move(before), std::move(after));

  // Keep the source pane's transform= attribute in lock-step with the DOM;
  // DocumentSyncController drains this queue once per frame.
  if (state.writebackTarget.has_value()) {
    liveApp.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
        .target = *state.writebackTarget,
        .transform = state.currentTransform,
    });
  }
}

}  // namespace donner::editor
