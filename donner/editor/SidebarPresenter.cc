#include "donner/editor/SidebarPresenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/css/CSS.h"
#include "donner/editor/DisclosureChevron.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LockState.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "embed_resources/BootstrapIcons.h"

namespace donner::editor {

InspectorStyleDisplayValue FormatInspectorStyleValue(std::string_view serializedValue) {
  constexpr std::string_view kDefaultSuffix = " (default)";
  constexpr std::string_view kSetSuffix = " (set)";

  InspectorStyleState state = InspectorStyleState::Unspecified;
  if (serializedValue.ends_with(kDefaultSuffix)) {
    serializedValue.remove_suffix(kDefaultSuffix.size());
    state = InspectorStyleState::Default;
  } else if (serializedValue.ends_with(kSetSuffix)) {
    serializedValue.remove_suffix(kSetSuffix.size());
    state = InspectorStyleState::Set;
  }

  const auto removeWrapper = [&](std::string_view prefix) {
    if (serializedValue.starts_with(prefix) && serializedValue.ends_with(')')) {
      serializedValue.remove_prefix(prefix.size());
      serializedValue.remove_suffix(1);
    }
  };
  removeWrapper("PaintServer(");
  if (serializedValue.starts_with("solid ")) {
    serializedValue.remove_prefix(std::string_view("solid ").size());
  }
  removeWrapper("Color(");

  return InspectorStyleDisplayValue{
      .value = serializedValue == "nullopt" ? "not set" : std::string(serializedValue),
      .state = state,
  };
}

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

bool CollectMarkerIds(const svg::SVGElement& root, std::vector<std::string>& ids) {
  constexpr std::size_t kMaximumVisitedNodes = 4096;
  constexpr std::size_t kMaximumMarkerIds = 100;
  std::deque<svg::SVGElement> pending{root};
  std::size_t visited = 0;
  bool truncated = false;
  while (!pending.empty() && visited < kMaximumVisitedNodes && ids.size() < kMaximumMarkerIds) {
    const svg::SVGElement element = pending.front();
    pending.pop_front();
    ++visited;
    const RcString tag = element.tagName().name;
    if (std::string_view(tag) == "marker") {
      const RcString id = element.id();
      const std::string_view name = id;
      if (!name.empty() && name.size() <= 100 && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-' || c == '.' || c == ':';
          })) {
        ids.emplace_back(name);
      }
    }
    for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
      if (pending.size() + visited >= kMaximumVisitedNodes) {
        truncated = true;
        break;
      }
      pending.push_back(*child);
    }
  }
  return truncated || !pending.empty();
}

bool IsValidStrokeDasharray(std::string_view value) {
  if (value == "none") {
    return true;
  }
  const std::string declaration = "stroke-dasharray: " + std::string(value);
  const std::vector<css::Declaration> parsed = css::CSS::ParseStyleAttribute(declaration);
  if (parsed.size() != 1 || parsed.front().name != "stroke-dasharray") {
    return false;
  }
  svg::PropertyRegistry properties;
  if (properties.parseProperty(parsed.front(), css::Specificity::StyleAttribute()).has_value()) {
    return false;
  }
  const auto pattern = properties.strokeDasharray.get();
  if (!pattern.has_value()) {
    return value == "none";
  }
  return !pattern->empty() &&
         std::all_of(pattern->begin(), pattern->end(), [](const Lengthd& part) {
           return std::isfinite(part.value) && part.value >= 0.0;
         });
}

enum class StrokeChoiceKind { Cap, Join };

bool RenderStrokeChoice(const char* id, const char* tooltip, StrokeChoiceKind kind, int index,
                        bool selected, const EditorTheme& theme, std::optional<Box2d>* rect) {
  const ImVec2 size(32.0f, 28.0f);
  const bool pressed = ImGui::InvisibleButton(id, size);
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  *rect = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 background = selected                 ? theme.surfaceActive
                           : ImGui::IsItemHovered() ? theme.surfaceHover
                                                    : theme.surfaceRaised;
  draw->AddRectFilled(min, max, background, theme.radiusControl);
  draw->AddRect(min, max, selected ? theme.accentDefault : theme.borderSubtle, theme.radiusControl);
  const ImU32 ink = selected ? theme.accentDefault : theme.textPrimary;
  const float centerY = (min.y + max.y) * 0.5f;
  if (kind == StrokeChoiceKind::Cap) {
    const float endX = min.x + 22.0f;
    draw->AddLine(ImVec2(min.x + 9.0f, centerY), ImVec2(endX, centerY), ink, 4.0f);
    if (index == 0) {
      draw->AddLine(ImVec2(endX, centerY - 5.0f), ImVec2(endX, centerY + 5.0f), ink, 1.0f);
    } else if (index == 1) {
      draw->AddCircleFilled(ImVec2(endX, centerY), 3.0f, ink);
    } else {
      draw->AddRectFilled(ImVec2(endX - 1.0f, centerY - 3.0f), ImVec2(endX + 4.0f, centerY + 3.0f),
                          ink);
    }
  } else {
    const ImVec2 apex(min.x + 16.0f, min.y + 6.0f);
    if (index == 3) {
      draw->AddLine(ImVec2(min.x + 9.0f, min.y + 21.0f), ImVec2(min.x + 13.0f, min.y + 12.0f), ink,
                    3.0f);
      draw->AddLine(ImVec2(min.x + 13.0f, min.y + 12.0f), ImVec2(min.x + 19.0f, min.y + 12.0f), ink,
                    3.0f);
      draw->AddLine(ImVec2(min.x + 19.0f, min.y + 12.0f), ImVec2(min.x + 23.0f, min.y + 21.0f), ink,
                    3.0f);
    } else {
      draw->AddLine(ImVec2(min.x + 9.0f, min.y + 21.0f), apex, ink, 3.0f);
      draw->AddLine(apex, ImVec2(min.x + 23.0f, min.y + 21.0f), ink, 3.0f);
      if (index == 2) {
        draw->AddCircleFilled(apex, 4.0f, ink);
      }
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return pressed;
}

bool RenderStrokeStep(const char* id, bool up, const EditorTheme& theme,
                      std::optional<Box2d>* rect) {
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(26.0f, 19.0f));
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  *rect = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(min, max, ImGui::IsItemHovered() ? theme.surfaceHover : theme.surfaceRaised,
                      theme.radiusControl);
  const float centerX = (min.x + max.x) * 0.5f;
  const float centerY = (min.y + max.y) * 0.5f;
  const float direction = up ? -1.0f : 1.0f;
  draw->AddTriangleFilled(ImVec2(centerX, centerY + direction * 4.0f),
                          ImVec2(centerX - 5.0f, centerY - direction * 2.0f),
                          ImVec2(centerX + 5.0f, centerY - direction * 2.0f), theme.textPrimary);
  return pressed;
}

std::vector<float> PreviewDashLengths(std::string_view pattern) {
  std::string normalized(pattern);
  std::replace(normalized.begin(), normalized.end(), ',', ' ');
  std::istringstream input(normalized);
  std::vector<float> values;
  float value = 0.0f;
  while (values.size() < 12u && input >> value) {
    if (!std::isfinite(value) || value < 0.0f) {
      return {};
    }
    values.push_back(value);
  }
  return input.eof() && !values.empty() ? values : std::vector<float>{};
}

void DrawDashPreview(ImDrawList* draw, const ImVec2& min, const ImVec2& max,
                     std::span<const float> lengths, ImU32 ink) {
  constexpr std::array<float, 2> kFallback = {6.0f, 4.0f};
  if (lengths.empty()) {
    lengths = kFallback;
  }
  float period = 0.0f;
  for (float length : lengths) {
    period += length;
  }
  const float scale = std::clamp(70.0f / std::max(period, 1.0f), 0.6f, 3.0f);
  const float right = max.x - 7.0f;
  const float centerY = (min.y + max.y) * 0.5f;
  float x = min.x + 7.0f;
  for (std::size_t index = 0; x < right && index < 100u; ++index) {
    const float length = std::max(2.0f, lengths[index % lengths.size()] * scale);
    if (index % 2u == 0u) {
      draw->AddLine(ImVec2(x, centerY), ImVec2(std::min(x + length, right), centerY), ink, 3.0f);
    }
    x += length;
  }
}

bool RenderDashPreset(const char* id, const char* tooltip, std::span<const float> lengths,
                      bool selected, const EditorTheme& theme, std::optional<Box2d>* rect) {
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(46.0f, 28.0f));
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  *rect = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(min, max, selected ? theme.surfaceActive : theme.surfaceRaised,
                      theme.radiusControl);
  draw->AddRect(min, max, selected ? theme.accentDefault : theme.borderSubtle, theme.radiusControl);
  DrawDashPreview(draw, min, max, lengths, selected ? theme.accentDefault : theme.textPrimary);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return pressed;
}

std::string MarkerDisplayLabel(std::string_view reference) {
  if (reference.starts_with("url(#") && reference.ends_with(')')) {
    return std::string(reference.substr(5, reference.size() - 6));
  }
  return reference == "none" ? "None" : std::string(reference);
}

void StrokeRowLabel(const char* label, float rowStartX, const EditorTheme& theme) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "%s", label);
  ImGui::SameLine();
  ImGui::SetCursorPosX(rowStartX + 70.0f);
}

void RenderRareJoinNote(int join, const EditorTheme& theme) {
  if (join != 1 && join != 4) {
    return;
  }
  ImGui::SameLine(0.0f, theme.space1);
  ImGui::TextDisabled("%s", join == 1 ? "Miter clip*" : "Arcs*");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Authored join renders as miter; choose a visible style to change it");
  }
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

enum class InspectorSectionKind : std::uint8_t {
  XmlAttributes,
  ComputedCss,
};

ImU32 PackInspectorColor(const css::RGBA& color) {
  return IM_COL32(color.r, color.g, color.b, color.a);
}

template <svg::PropertyCascade kCascade>
std::optional<ImU32> InspectorPaintSwatch(const svg::Property<svg::PaintServer, kCascade>& property,
                                          const css::RGBA& currentColor) {
  const std::optional<svg::PaintServer> paint = property.get();
  if (!paint.has_value() || !paint->is<svg::PaintServer::Solid>()) {
    return std::nullopt;
  }

  return PackInspectorColor(
      paint->get<svg::PaintServer::Solid>().color.resolve(currentColor, 1.0f));
}

void RenderInspectorColorSwatch(ImU32 color) {
  const EditorTheme& theme = EditorTheme::Active();
  constexpr float kSwatchSize = 12.0f;
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const float lineHeight = ImGui::GetTextLineHeight();
  const ImVec2 min(cursor.x, cursor.y + (lineHeight - kSwatchSize) * 0.5f);
  const ImVec2 max(min.x + kSwatchSize, min.y + kSwatchSize);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(min, max, color, theme.radiusControl * 0.5f);
  drawList->AddRect(min, max, theme.borderStrong, theme.radiusControl * 0.5f);
  ImGui::Dummy(ImVec2(kSwatchSize, lineHeight));
  ImGui::SameLine(0.0f, theme.space2);
}

void RenderInspectorValue(std::string_view value, ImU32 textColor,
                          std::optional<ImU32> swatchColor) {
  if (swatchColor.has_value()) {
    RenderInspectorColorSwatch(*swatchColor);
  }

  const float availableWidth = ImGui::GetContentRegionAvail().x;
  const bool clipped =
      ImGui::CalcTextSize(value.data(), value.data() + value.size()).x > availableWidth;
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(textColor));
  ImGui::TextUnformatted(value.data(), value.data() + value.size());
  ImGui::PopStyleColor();
  if (clipped && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%.*s", static_cast<int>(value.size()), value.data());
  }
}

void RenderInspectorSection(const char* heading, const char* tableId,
                            std::span<const std::pair<std::string, std::string>> entries,
                            InspectorSectionKind kind,
                            std::span<const std::optional<ImU32>> swatches = {}) {
  const EditorTheme& theme = EditorTheme::Active();
  const bool computedCss = kind == InspectorSectionKind::ComputedCss;

  ImGui::Separator();
  ImGui::Dummy(ImVec2(0.0f, theme.space1));
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textPrimary), "%s", heading);
  const std::string count = std::to_string(entries.size());
  const float countWidth = ImGui::CalcTextSize(count.c_str()).x;
  const float countX = ImGui::GetWindowContentRegionMax().x - countWidth;
  if (countX > ImGui::GetCursorPosX() + theme.space2) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(countX);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textDisabled), "%s", count.c_str());
  }

  if (entries.empty()) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textDisabled), "%s",
                       computedCss ? "No computed properties" : "No attributes");
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX |
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_NoSavedSettings;
  const int columnCount = computedCss ? 3 : 2;
  const float availableWidth = ImGui::GetContentRegionAvail().x;
  const float nameWidth = std::clamp(availableWidth * 0.34f, 76.0f, 124.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(theme.space2, theme.space1));
  if (ImGui::BeginTable(tableId, columnCount, kFlags)) {
    ImGui::TableSetupColumn("##inspector_property_name", ImGuiTableColumnFlags_WidthFixed,
                            nameWidth);
    ImGui::TableSetupColumn("##inspector_property_value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    if (computedCss) {
      ImGui::TableSetupColumn("##inspector_property_state", ImGuiTableColumnFlags_WidthFixed,
                              66.0f);
    }

    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto& [name, value] = entries[index];
      const InspectorStyleDisplayValue cssValue = computedCss
                                                      ? FormatInspectorStyleValue(value)
                                                      : InspectorStyleDisplayValue{.value = value};
      const bool isDefault = cssValue.state == InspectorStyleState::Default;
      const std::optional<ImU32> swatch = index < swatches.size() ? swatches[index] : std::nullopt;

      ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeight() + theme.space2);
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(
          ImGui::ColorConvertU32ToFloat4(isDefault ? theme.textDisabled : theme.textMuted), "%s",
          name.c_str());
      ImGui::TableSetColumnIndex(1);
      RenderInspectorValue(cssValue.value, isDefault ? theme.textMuted : theme.textPrimary, swatch);
      if (computedCss) {
        ImGui::TableSetColumnIndex(2);
        if (cssValue.state != InspectorStyleState::Unspecified) {
          const bool isSet = cssValue.state == InspectorStyleState::Set;
          ImGui::TextColored(
              ImGui::ColorConvertU32ToFloat4(isSet ? theme.accentDefault : theme.textDisabled),
              "%s", isSet ? "SET" : "DEFAULT");
        }
      }
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
}

struct PathOperationButton {
  PathOperationKind operation;
  const char* id;
  const char* tooltip;
};

constexpr std::array<PathOperationButton, kInspectorPathOperations.size()> kPathOperationButtons = {
    {
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

const std::optional<svg::RendererBitmap>& CachedPathOperationIconBitmap(
    PathOperationKind operation) {
  switch (operation) {
    case PathOperationKind::Union: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          PathOperationIconSvg(PathOperationKind::Union), kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::Intersect: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          PathOperationIconSvg(PathOperationKind::Intersect), kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::SubtractFront: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          PathOperationIconSvg(PathOperationKind::SubtractFront), kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::SubtractBack: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          PathOperationIconSvg(PathOperationKind::SubtractBack), kPathOperationIconRasterSizePx);
      return bitmap;
    }
    case PathOperationKind::Exclude: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          PathOperationIconSvg(PathOperationKind::Exclude), kPathOperationIconRasterSizePx);
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
                               std::span<const PathOperationAvailability> snapshotAvailability,
                               const SidebarPresenter::IconTextureProvider& iconTextureProvider) {
  ImGui::Separator();
  ImGui::TextUnformatted("Path Operations");

  bool queuedMutation = false;
  for (std::size_t i = 0; i < kPathOperationButtons.size(); ++i) {
    const PathOperationButton& button = kPathOperationButtons[i];
    const PathOperationAvailability availability =
        liveApp != nullptr ? liveApp->pathOperationAvailability(button.operation)
        : i < snapshotAvailability.size()
            ? snapshotAvailability[i]
            : PathOperationAvailability{.canApply = false, .reason = "Unavailable"};

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

Lengthd FirstSelectedStrokeWidth(const EditorApp& app) {
  const std::vector<svg::SVGElement>& selection = app.selectedElements();
  if (selection.empty()) {
    return Lengthd(1.0);
  }

  return selection.front().getComputedStyle().strokeWidth.get().value();
}

/// Below this document-space span (in user units), a bounds axis is treated
/// as degenerate and the matching Width / Height field is disabled - a scale
/// factor against a ~zero span would explode.
constexpr double kMinimumSpanForScale = 1e-6;

}  // namespace

std::span<const unsigned char> PathOperationIconSvg(PathOperationKind operation) {
  switch (operation) {
    case PathOperationKind::Union: return embedded::kBootstrapUnionSvg;
    case PathOperationKind::Intersect: return embedded::kBootstrapIntersectSvg;
    case PathOperationKind::SubtractFront: return embedded::kBootstrapSubtractSvg;
    // No inspector button today; the front/back distinction is a z-order
    // argument, not a different boolean, so both reuse the subtract glyph.
    case PathOperationKind::SubtractBack: return embedded::kBootstrapSubtractSvg;
    case PathOperationKind::Exclude: return embedded::kBootstrapExcludeSvg;
  }
  return embedded::kBootstrapUnionSvg;
}

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

std::span<const EmbeddedSvgIconRequest> SidebarIconPrewarmRequests() {
  static const std::array<EmbeddedSvgIconRequest, 5> kRequests = {{
      {PathOperationIconSvg(PathOperationKind::Union), kPathOperationIconRasterSizePx,
       /*tintableMask=*/true},
      {PathOperationIconSvg(PathOperationKind::Intersect), kPathOperationIconRasterSizePx,
       /*tintableMask=*/true},
      {PathOperationIconSvg(PathOperationKind::SubtractFront), kPathOperationIconRasterSizePx,
       /*tintableMask=*/true},
      {PathOperationIconSvg(PathOperationKind::SubtractBack), kPathOperationIconRasterSizePx,
       /*tintableMask=*/true},
      {PathOperationIconSvg(PathOperationKind::Exclude), kPathOperationIconRasterSizePx,
       /*tintableMask=*/true},
  }};
  return kRequests;
}

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

Transform2d ComposeTransform(const DecomposedTransform& decomposed) {
  return Transform2d::Scale(decomposed.scale) * Transform2d::Rotate(decomposed.rotationRadians) *
         Transform2d::Translate(decomposed.translation);
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

void SidebarPresenter::refreshMarkerCache(const EditorApp& app) {
  const svg::SVGElement rootElement = app.document().document().svgElement();
  const std::uint64_t sourceVersion = app.document().document().sourceVersion();
  const std::string_view sourceText = app.document().document().source();
  const bool sourceChanged = sourceVersion == 0 ? markerCacheSourceText_ != sourceText
                                                : markerCacheSourceVersion_ != sourceVersion;
  if (!markerCacheRoot_.has_value() || *markerCacheRoot_ != rootElement || sourceChanged ||
      sourceText.empty()) {
    markerCacheIds_.clear();
    markerCacheTruncated_ = CollectMarkerIds(rootElement, markerCacheIds_);
    markerCacheRoot_ = rootElement;
    markerCacheSourceVersion_ = sourceVersion;
    markerCacheSourceText_ = sourceVersion == 0 ? std::string(sourceText) : std::string();
    ++markerScanCount_;
  }
}

void SidebarPresenter::captureStrokeSnapshot(const EditorApp& app,
                                             std::span<const svg::SVGElement> selection,
                                             InspectorSnapshot& inspector) {
  const bool hasSelection = !selection.empty();
  inspector.strokeEditable =
      hasSelection && std::ranges::none_of(selection, [](const svg::SVGElement& element) {
        return IsLocked(element);
      });
  if (!hasSelection) {
    return;
  }
  const auto& stroke = selection.front().getComputedStyle();
  inspector.strokeWidth = stroke.strokeWidth.get().value();
  inspector.strokeLinecap = static_cast<int>(stroke.strokeLinecap.get().value());
  inspector.strokeLinejoin = static_cast<int>(stroke.strokeLinejoin.get().value());
  inspector.strokeMiterlimit = static_cast<float>(stroke.strokeMiterlimit.get().value());
  if (const auto dasharray = stroke.strokeDasharray.get(); dasharray.has_value()) {
    std::ostringstream stream;
    stream << *dasharray;
    inspector.strokeDasharray = stream.str();
  }
  inspector.strokeDashoffset = stroke.strokeDashoffset.get().value();
  if (const auto marker = stroke.markerStart.get(); marker.has_value()) {
    inspector.markerStart = "url(" + std::string(std::string_view(marker->href)) + ")";
  }
  if (const auto marker = stroke.markerEnd.get(); marker.has_value()) {
    inspector.markerEnd = "url(" + std::string(std::string_view(marker->href)) + ")";
  }
  refreshMarkerCache(app);
  inspector.markerIds = markerCacheIds_;
  inspector.markerListTruncated = markerCacheTruncated_;
}

void SidebarPresenter::refreshSnapshot(const EditorApp& app) {
  if (!app.hasDocument()) {
    treeSnapshot_.reset();
    inspectorSnapshot_ = InspectorSnapshot{};
    // The document is gone; the edit's element handle and baseline are
    // meaningless now, so drop the in-progress edit instead of committing.
    transformEdit_.reset();
    markerCacheRoot_.reset();
    markerCacheIds_.clear();
    markerCacheSourceVersion_ = 0;
    markerCacheSourceText_.clear();
    markerCacheTruncated_ = false;
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
  captureStrokeSnapshot(app, selectionList, inspector);
  inspector.pathOperationAvailability.reserve(kPathOperationButtons.size());
  for (const PathOperationButton& button : kPathOperationButtons) {
    inspector.pathOperationAvailability.push_back(app.pathOperationAvailability(button.operation));
  }
  if (selectionList.size() == 1) {
    inspector.hasSelection = true;
    const donner::svg::SVGElement& selected = selectionList.front();
    inspector.transformEditable = !IsLocked(selected);
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
    inspector.computedStyleSwatches.reserve(9);
    const css::RGBA fallbackCurrentColor = css::RGBA::RGB(0, 0, 0);
    const std::optional<css::Color> computedColor = computedStyle.color.get();
    const css::RGBA currentColor = computedColor.has_value()
                                       ? computedColor->resolve(fallbackCurrentColor, 1.0f)
                                       : fallbackCurrentColor;
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.display);
    inspector.computedStyleSwatches.push_back(std::nullopt);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.visibility);
    inspector.computedStyleSwatches.push_back(std::nullopt);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.opacity);
    inspector.computedStyleSwatches.push_back(std::nullopt);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.fill);
    inspector.computedStyleSwatches.push_back(
        InspectorPaintSwatch(computedStyle.fill, currentColor));
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.fillOpacity);
    inspector.computedStyleSwatches.push_back(std::nullopt);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.stroke);
    inspector.computedStyleSwatches.push_back(
        InspectorPaintSwatch(computedStyle.stroke, currentColor));
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.strokeWidth);
    inspector.computedStyleSwatches.push_back(std::nullopt);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.strokeOpacity);
    inspector.computedStyleSwatches.push_back(std::nullopt);
    AppendComputedStyleEntry(inspector.computedStyle, computedStyle.color);
    inspector.computedStyleSwatches.push_back(PackInspectorColor(currentColor));
  }
  inspectorSnapshot_ = std::move(inspector);
}

void SidebarPresenter::toggleTreeNodeExpanded(std::uint32_t entityId) const {
  if (const auto it = treeExpandedEntities_.find(entityId); it != treeExpandedEntities_.end()) {
    treeExpandedEntities_.erase(it);
  } else {
    treeExpandedEntities_.insert(entityId);
  }
}

void SidebarPresenter::renderTreeNode(EditorApp* liveApp, const TreeNodeSnapshot& node,
                                      TreeViewState& state,
                                      const IconTextureProvider& iconTextureProvider) const {
  const bool hasChildren = !node.children.empty();
  const std::uint32_t entityId =
      static_cast<std::uint32_t>(node.element->unsafeEntityHandle().entity());

  // Auto-expand ancestors of a pending scroll target so the selection is
  // revealed, matching the former SetNextItemOpen behavior.
  const bool onSelectionPath = state.pendingScroll && state.scrollTarget.has_value() &&
                               IsAncestorOrSelf(*node.element, *state.scrollTarget);
  if (hasChildren && onSelectionPath) {
    treeExpandedEntities_.insert(entityId);
  }
  const bool expanded = hasChildren && treeExpandedEntities_.count(entityId) != 0;

  ImGui::PushID(static_cast<int>(entityId));

  const float rowHeight = ImGui::GetFrameHeight();
  const float chevronCell = rowHeight;

  // Disclosure chevron shared with the LayersPanel: a right-pointing chevron
  // when collapsed, rotated to point down when expanded. Non-expandable rows
  // get a same-width spacer so labels stay aligned. Clicking the chevron
  // toggles the persistent disclosure state (only when a live app is present,
  // matching the selection click gating).
  if (hasChildren) {
    const ImVec2 cellMin = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##disclosure", ImVec2(chevronCell, rowHeight)) &&
        liveApp != nullptr) {
      toggleTreeNodeExpanded(entityId);
    }
    if (iconTextureProvider) {
      const std::optional<svg::RendererBitmap>& bitmap = CachedDisclosureChevronBitmap(expanded);
      if (bitmap.has_value()) {
        const IconTexture chevronTexture = iconTextureProvider(
            0xf600000000000000ull + 9u +
                static_cast<std::uint64_t>(DisclosureChevronTextureVariant(expanded)),
            *bitmap);
        const ImVec2 center(cellMin.x + chevronCell * 0.5f, cellMin.y + rowHeight * 0.5f);
        DrawDisclosureChevron(ImGui::GetWindowDrawList(), chevronTexture.texture,
                              chevronTexture.uvBottomRight, center, 11.0f,
                              ImGui::GetColorU32(ImGuiCol_Text));
      }
    }
    ImGui::SameLine(0.0f, 2.0f);
  } else {
    ImGui::Dummy(ImVec2(chevronCell, 0.0f));
    ImGui::SameLine(0.0f, 2.0f);
  }

  // Row label + selection highlight. A default-size selectable already spans
  // the remaining content width, giving the same hit box and highlight the tree
  // node used to.
  if (ImGui::Selectable(node.label.c_str(), node.isSelected) && liveApp != nullptr) {
    const bool toggleSelection = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    if (toggleSelection) {
      liveApp->toggleInSelection(*node.element);
    } else {
      liveApp->setSelection(*node.element);
    }
    state.selectionChangedInTree = true;
    state.pendingScroll = false;
  }

  ImGui::PopID();

  if (expanded) {
    ImGui::Indent(chevronCell);
    for (const auto& child : node.children) {
      renderTreeNode(liveApp, child, state, iconTextureProvider);
    }
    ImGui::Unindent(chevronCell);
  }
}

void SidebarPresenter::renderTreeView(EditorApp* liveApp, TreeViewState& state,
                                      const IconTextureProvider& iconTextureProvider) const {
  if (!treeSnapshot_.has_value()) {
    ImGui::TextDisabled("(no document)");
    return;
  }
  renderTreeNode(liveApp, *treeSnapshot_, state, iconTextureProvider);
}

bool SidebarPresenter::renderInspector(EditorApp* liveApp, const ViewportState&,
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
      queuedMutation = renderStrokeControlsPanel(liveApp);
      queuedMutation =
          RenderPathOperationsPanel(liveApp, inspectorSnapshot_.pathOperationAvailability,
                                    iconTextureProvider) ||
          queuedMutation;
    } else {
      ImGui::TextDisabled("Select a single element to inspect attributes.");
      queuedMutation = renderStrokeControlsPanel(liveApp);
      queuedMutation =
          RenderPathOperationsPanel(liveApp, inspectorSnapshot_.pathOperationAvailability,
                                    iconTextureProvider) ||
          queuedMutation;
    }
  } else {
    const EditorTheme& theme = EditorTheme::Active();
    constexpr std::string_view kSelectedPrefix = "Selected: ";
    std::string_view elementLabel = inspectorSnapshot_.titleText;
    if (elementLabel.starts_with(kSelectedPrefix)) {
      elementLabel.remove_prefix(kSelectedPrefix.size());
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "ELEMENT");
    ImGui::SameLine(0.0f, theme.space2);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.accentDefault), "%.*s",
                       static_cast<int>(elementLabel.size()), elementLabel.data());
    if (inspectorSnapshot_.bounds.has_value()) {
      const auto& b = *inspectorSnapshot_.bounds;
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted),
                         "Bounds  %.1f x %.1f at %.1f, %.1f", b.width(), b.height(), b.topLeft.x,
                         b.topLeft.y);
    }
    queuedMutation = renderTransformPanel(liveApp);
    queuedMutation = renderStrokeControlsPanel(liveApp) || queuedMutation;
    RenderInspectorSection("XML attributes", "##inspector_xml_attributes",
                           inspectorSnapshot_.xmlAttributes, InspectorSectionKind::XmlAttributes);
    RenderInspectorSection("Computed CSS", "##inspector_computed_style",
                           inspectorSnapshot_.computedStyle, InspectorSectionKind::ComputedCss,
                           inspectorSnapshot_.computedStyleSwatches);
    queuedMutation =
        RenderPathOperationsPanel(liveApp, inspectorSnapshot_.pathOperationAvailability,
                                  iconTextureProvider) ||
        queuedMutation;
  }
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeControlsPanel(EditorApp* liveApp) {
  const EditorTheme& theme = EditorTheme::Active();
  ImGui::Separator();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "STROKE");
  const bool canMutate =
      liveApp != nullptr && liveApp->hasSelection() && inspectorSnapshot_.strokeEditable;
  if (liveApp != nullptr && strokeScalarEdit_.has_value() && strokeScalarEdit_->pendingCommit) {
    finishStrokeScalarEdit(*liveApp, strokeScalarEdit_->field);
  }
  ImGui::BeginDisabled(!inspectorSnapshot_.strokeEditable);
  const StrokeRenderContext context{liveApp, theme, ImGui::GetCursorPosX(), canMutate};
  bool queuedMutation = renderStrokeWidthRow(context);
  queuedMutation = renderStrokeCapRow(context) || queuedMutation;
  queuedMutation = renderStrokeJoinRow(context) || queuedMutation;
  queuedMutation = renderStrokeMiterRow(context) || queuedMutation;
  ImGui::Separator();
  queuedMutation = renderStrokeDashSection(context) || queuedMutation;
  ImGui::Separator();
  queuedMutation = renderStrokeMarkers(context) || queuedMutation;
  ImGui::EndDisabled();
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeWidthRow(const StrokeRenderContext& context) {
  const EditorTheme& theme = context.theme;
  StrokeRowLabel("Width", context.rowStartX, theme);
  const Lengthd widthLength =
      context.canMutate ? FirstSelectedStrokeWidth(*context.app) : inspectorSnapshot_.strokeWidth;
  float width = static_cast<float>(widthLength.value);
  bool queuedMutation = renderStrokeWidthField(context, widthLength, &width);
  ImGui::SameLine(0.0f, theme.space1);
  queuedMutation = renderStrokeWidthStepper(context, widthLength, width) || queuedMutation;
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeWidthField(const StrokeRenderContext& context,
                                              const Lengthd& widthLength, float* width) {
  EditorApp* liveApp = context.app;
  const bool canMutate = context.canMutate;
  std::ostringstream widthUnit;
  widthUnit << widthLength.unit;
  const std::string widthFormat = "%.1f" + widthUnit.str();
  ImGui::SetNextItemWidth(90.0f);
  const bool widthChanged =
      ImGui::DragFloat("##stroke_width", width, 0.1f, 0.0f, 200.0f, widthFormat.c_str());
  {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    strokeWidthRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  }
  bool queuedMutation = false;
  if (widthChanged && canMutate) {
    beginStrokeScalarEdit(*liveApp, StrokeScalarField::Width);
    if (widthLength.unit == LengthUnit::None || widthLength.unit == LengthUnit::Px) {
      liveApp->setActiveStrokeWidth(*width);
    }
    const RcString cssWidth = Lengthd(*width, widthLength.unit).toRcString();
    const bool changed = liveApp->setStylePropertyOnSelection("stroke-width", cssWidth);
    strokeScalarEdit_->changed |= changed;
    queuedMutation = changed;
  }
  trackStrokeScalarItem(context, StrokeScalarField::Width);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(widthLength.unit == LengthUnit::None ? "Stroke width in user units"
                                                           : "Stroke width retains its SVG unit");
  }
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeWidthStepper(const StrokeRenderContext& context,
                                                const Lengthd& widthLength, float width) {
  EditorApp* liveApp = context.app;
  const EditorTheme& theme = context.theme;
  const bool canMutate = context.canMutate;
  bool queuedMutation = false;
  ImGui::BeginGroup();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 1.0f));
  if (RenderStrokeStep("##stroke_width_up", true, theme, &strokeIncrementRect_) && canMutate) {
    width += 1.0f;
    if (widthLength.unit == LengthUnit::None || widthLength.unit == LengthUnit::Px) {
      liveApp->setActiveStrokeWidth(width);
    }
    const RcString cssWidth = Lengthd(width, widthLength.unit).toRcString();
    queuedMutation = applyStrokeStyle(*liveApp, "stroke-width", cssWidth, "Change stroke width") ||
                     queuedMutation;
  }
  std::optional<Box2d> decrementRect;
  if (RenderStrokeStep("##stroke_width_down", false, theme, &decrementRect) && canMutate) {
    width = std::max(0.0f, width - 1.0f);
    if (widthLength.unit == LengthUnit::None || widthLength.unit == LengthUnit::Px) {
      liveApp->setActiveStrokeWidth(width);
    }
    const RcString cssWidth = Lengthd(width, widthLength.unit).toRcString();
    queuedMutation = applyStrokeStyle(*liveApp, "stroke-width", cssWidth, "Change stroke width") ||
                     queuedMutation;
  }
  ImGui::PopStyleVar();
  ImGui::EndGroup();
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeCapRow(const StrokeRenderContext& context) {
  EditorApp* liveApp = context.app;
  const EditorTheme& theme = context.theme;
  const float rowStartX = context.rowStartX;
  const bool canMutate = context.canMutate;
  bool queuedMutation = false;
  constexpr std::array<const char*, 3> kCaps = {"butt", "round", "square"};
  const int cap = std::clamp(inspectorSnapshot_.strokeLinecap, 0, 2);
  StrokeRowLabel("Cap", rowStartX, theme);
  ImGui::PushID("stroke_caps");
  for (int index = 0; index < static_cast<int>(kCaps.size()); ++index) {
    if (index > 0) {
      ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::PushID(index);
    if (RenderStrokeChoice("##choice", kCaps[index], StrokeChoiceKind::Cap, index, cap == index,
                           theme, &strokeCapRects_[index]) &&
        canMutate) {
      queuedMutation =
          applyStrokeStyle(*liveApp, "stroke-linecap", kCaps[index], "Change stroke cap") ||
          queuedMutation;
    }
    ImGui::PopID();
  }
  ImGui::PopID();

  return queuedMutation;
}

bool SidebarPresenter::renderStrokeJoinRow(const StrokeRenderContext& context) {
  EditorApp* liveApp = context.app;
  const EditorTheme& theme = context.theme;
  const float rowStartX = context.rowStartX;
  const bool canMutate = context.canMutate;
  bool queuedMutation = false;
  constexpr std::array<const char*, 5> kJoins = {"miter", "miter-clip", "round", "bevel", "arcs"};
  const int join = std::clamp(inspectorSnapshot_.strokeLinejoin, 0, 4);
  StrokeRowLabel("Join", rowStartX, theme);
  strokeJoinRects_.fill(std::nullopt);
  ImGui::PushID("stroke_joins");
  constexpr std::array<int, 3> kCommonJoins = {0, 2, 3};
  for (std::size_t position = 0; position < kCommonJoins.size(); ++position) {
    const int index = kCommonJoins[position];
    if (position > 0u) {
      ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::PushID(index);
    if (RenderStrokeChoice("##choice", kJoins[index], StrokeChoiceKind::Join, index,
                           join == index || (index == 0 && (join == 1 || join == 4)), theme,
                           &strokeJoinRects_[index]) &&
        canMutate) {
      queuedMutation =
          applyStrokeStyle(*liveApp, "stroke-linejoin", kJoins[index], "Change stroke join") ||
          queuedMutation;
    }
    ImGui::PopID();
  }
  RenderRareJoinNote(join, theme);
  ImGui::PopID();

  return queuedMutation;
}

bool SidebarPresenter::renderStrokeMiterRow(const StrokeRenderContext& context) {
  EditorApp* liveApp = context.app;
  const EditorTheme& theme = context.theme;
  const float rowStartX = context.rowStartX;
  const bool canMutate = context.canMutate;
  bool queuedMutation = false;
  const int join = std::clamp(inspectorSnapshot_.strokeLinejoin, 0, 4);
  strokeMiterLimitRect_.reset();
  if (join == 0 || join == 1 || join == 4) {
    StrokeRowLabel("Limit", rowStartX, theme);
    float miterlimit = inspectorSnapshot_.strokeMiterlimit;
    ImGui::SetNextItemWidth(90.0f);
    const bool limitChanged =
        ImGui::InputFloat("##stroke_miter_limit", &miterlimit, 0.1f, 1.0f, "%.1f");
    if (limitChanged && canMutate) {
      if (std::isfinite(miterlimit) && miterlimit >= 1.0f) {
        beginStrokeScalarEdit(*liveApp, StrokeScalarField::MiterLimit);
        const bool changed =
            liveApp->setStylePropertyOnSelection("stroke-miterlimit", std::to_string(miterlimit));
        strokeScalarEdit_->changed |= changed;
        queuedMutation = changed || queuedMutation;
        strokeMiterError_.clear();
      } else {
        strokeMiterError_ = "Limit must be at least 1";
      }
    }
    trackStrokeScalarItem(context, StrokeScalarField::MiterLimit);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    strokeMiterLimitRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Maximum sharp-corner length relative to stroke width");
    }
    if (!strokeMiterError_.empty()) {
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.destructive), "%s",
                         strokeMiterError_.c_str());
    }
  }

  return queuedMutation;
}

bool SidebarPresenter::renderStrokeDashSection(const StrokeRenderContext& context) {
  const bool snapshotDashed =
      inspectorSnapshot_.strokeDasharray != "none" && !inspectorSnapshot_.strokeDasharray.empty();
  if (snapshotDashed) {
    lastDashPattern_ = inspectorSnapshot_.strokeDasharray;
  }
  bool dashed = snapshotDashed;
  bool queuedMutation = false;
  if (ImGui::Checkbox("Dashed line", &dashed)) {
    if (context.canMutate) {
      queuedMutation = applyStrokeStyle(*context.app, "stroke-dasharray",
                                        dashed ? lastDashPattern_ : "none", "Toggle dashed stroke");
    } else {
      dashed = snapshotDashed;
    }
  }
  const ImVec2 toggleMin = ImGui::GetItemRectMin();
  const ImVec2 toggleMax = ImGui::GetItemRectMax();
  strokeDashToggleRect_ =
      Box2d(Vector2d(toggleMin.x, toggleMin.y), Vector2d(toggleMax.x, toggleMax.y));
  strokeDashPreviewRect_.reset();
  strokeDashPresetRects_.fill(std::nullopt);
  strokeDashOffsetRect_.reset();
  strokeCustomDashSelected_ = false;
  if (!dashed) {
    strokeDasharrayEditing_ = false;
    return queuedMutation;
  }

  const std::string_view pattern =
      snapshotDashed ? inspectorSnapshot_.strokeDasharray : lastDashPattern_;
  const std::vector<float> currentLengths = PreviewDashLengths(pattern);
  renderDashPreviewRow(context, currentLengths, pattern);
  queuedMutation = renderDashPresetRow(context, currentLengths) || queuedMutation;
  queuedMutation = renderDashCustomEditor(context, pattern) || queuedMutation;
  queuedMutation = renderDashOffsetRow(context) || queuedMutation;
  return queuedMutation;
}

void SidebarPresenter::renderDashPreviewRow(const StrokeRenderContext& context,
                                            std::span<const float> lengths,
                                            std::string_view pattern) {
  const EditorTheme& theme = context.theme;
  StrokeRowLabel("Preview", context.rowStartX, theme);
  ImGui::Dummy(ImVec2(174.0f, 24.0f));
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  strokeDashPreviewRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(min, max, theme.surfaceSunken, theme.radiusControl);
  draw->AddRect(min, max, theme.borderSubtle, theme.radiusControl);
  DrawDashPreview(draw, min, max, lengths, theme.accentDefault);
  if (ImGui::IsItemHovered()) {
    const std::size_t previewLength = std::min(pattern.size(), std::size_t{80});
    ImGui::SetTooltip("Dash pattern: %.*s", static_cast<int>(previewLength), pattern.data());
  }
}

bool SidebarPresenter::renderDashPresetRow(const StrokeRenderContext& context,
                                           std::span<const float> currentLengths) {
  const EditorTheme& theme = context.theme;
  StrokeRowLabel("Style", context.rowStartX, theme);
  constexpr std::array<std::string_view, 3> kPresetValues = {"6 4", "1 4", "6 3 1 3"};
  constexpr std::array<const char*, 3> kPresetNames = {"Dash", "Dot", "Dash-dot"};
  ImGui::PushID("stroke_dash_presets");
  bool matchedPreset = false;
  bool queuedMutation = false;
  for (std::size_t index = 0; index < kPresetValues.size(); ++index) {
    if (index > 0) {
      ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::PushID(static_cast<int>(index));
    const std::vector<float> presetLengths = PreviewDashLengths(kPresetValues[index]);
    const bool selected = std::equal(currentLengths.begin(), currentLengths.end(),
                                     presetLengths.begin(), presetLengths.end());
    matchedPreset |= selected;
    if (RenderDashPreset("##choice", kPresetNames[index], presetLengths, selected, theme,
                         &strokeDashPresetRects_[index]) &&
        context.canMutate) {
      queuedMutation = submitDashPattern(*context.app, kPresetValues[index]) || queuedMutation;
    }
    ImGui::PopID();
  }
  ImGui::PopID();
  ImGui::SameLine(0.0f, theme.space1);
  strokeCustomDashSelected_ = !matchedPreset;
  if (strokeCustomDashSelected_) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme.surfaceActive));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.accentDefault));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme.accentDefault));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  }
  if (ImGui::SmallButton(strokeDashDetailsOpen_ ? "Done##stroke_dash" : "Custom##stroke_dash")) {
    strokeDashDetailsOpen_ = !strokeDashDetailsOpen_;
  }
  if (strokeCustomDashSelected_) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
  }
  if (!strokeDashDetailsOpen_) {
    strokeDasharrayEditing_ = false;
  }
  return queuedMutation;
}

bool SidebarPresenter::renderDashCustomEditor(const StrokeRenderContext& context,
                                              std::string_view pattern) {
  if (!strokeDashDetailsOpen_) {
    return false;
  }
  const EditorTheme& theme = context.theme;
  bool queuedMutation = false;
  if (inspectorSnapshot_.strokeDasharray.size() >= strokeDasharrayBuffer_.size()) {
    strokeDasharrayEditing_ = false;
    ImGui::TextDisabled("Long pattern - edit in SVG source");
  } else {
    if (!strokeDasharrayEditing_) {
      strokeDasharrayBuffer_.fill('\0');
      std::copy(pattern.begin(), pattern.end(), strokeDasharrayBuffer_.begin());
    }
    StrokeRowLabel("Pattern", context.rowStartX, theme);
    ImGui::SetNextItemWidth(174.0f);
    const bool submitted =
        ImGui::InputText("##stroke_dash_pattern", strokeDasharrayBuffer_.data(),
                         strokeDasharrayBuffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    strokeDasharrayEditing_ = ImGui::IsItemActive();
    if (submitted && context.canMutate) {
      queuedMutation = submitDashPattern(*context.app, strokeDasharrayBuffer_.data());
      strokeDashError_ = queuedMutation ? "" : "Use nonnegative dash and gap lengths";
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Dash and gap lengths, e.g. 4 2 or 4 2 1 2; press Enter to apply");
    }
  }
  if (!strokeDashError_.empty()) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.destructive), "%s",
                       strokeDashError_.c_str());
  }
  return queuedMutation;
}

bool SidebarPresenter::renderDashOffsetRow(const StrokeRenderContext& context) {
  if (!strokeDashDetailsOpen_ && inspectorSnapshot_.strokeDashoffset.value == 0.0) {
    return false;
  }
  StrokeRowLabel("Offset", context.rowStartX, context.theme);
  const Lengthd offsetLength = inspectorSnapshot_.strokeDashoffset;
  float dashoffset = static_cast<float>(offsetLength.value);
  std::ostringstream offsetUnit;
  offsetUnit << offsetLength.unit;
  const std::string offsetFormat = "%.1f" + offsetUnit.str();
  ImGui::SetNextItemWidth(90.0f);
  const bool offsetChanged =
      ImGui::InputFloat("##stroke_dash_offset", &dashoffset, 0.1f, 1.0f, offsetFormat.c_str());
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  strokeDashOffsetRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  bool queuedMutation = false;
  if (offsetChanged && context.canMutate && std::isfinite(dashoffset)) {
    beginStrokeScalarEdit(*context.app, StrokeScalarField::DashOffset);
    queuedMutation = setStrokeDashOffset(*context.app, dashoffset);
    strokeScalarEdit_->changed |= queuedMutation;
  }
  trackStrokeScalarItem(context, StrokeScalarField::DashOffset);
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeMarkers(const StrokeRenderContext& context) {
  bool queuedMutation = false;
  const bool hasMarker =
      inspectorSnapshot_.markerStart != "none" || inspectorSnapshot_.markerEnd != "none";
  ImGui::SetNextItemOpen(hasMarker, ImGuiCond_Once);
  const bool markersOpen = ImGui::TreeNodeEx("Markers", ImGuiTreeNodeFlags_SpanAvailWidth);
  if (markersOpen) {
    queuedMutation =
        renderStrokeMarkerPicker(context, "Start", "marker-start", inspectorSnapshot_.markerStart);
    queuedMutation =
        renderStrokeMarkerPicker(context, "End", "marker-end", inspectorSnapshot_.markerEnd) ||
        queuedMutation;
    if (inspectorSnapshot_.markerListTruncated) {
      ImGui::TextDisabled("Marker list limited to the first 100 IDs in 4096 SVG nodes.");
    }
    ImGui::TreePop();
  }

  return queuedMutation;
}

bool SidebarPresenter::renderStrokeMarkerPicker(const StrokeRenderContext& context,
                                                const char* label, const char* property,
                                                const std::string& current) {
  StrokeRowLabel(label, context.rowStartX, context.theme);
  const std::string display = MarkerDisplayLabel(current);
  ImGui::SetNextItemWidth(144.0f);
  ImGui::PushID(property);
  bool changed = false;
  if (ImGui::BeginCombo("##marker", display.c_str())) {
    if (ImGui::Selectable("None", current == "none") && context.canMutate) {
      changed = applyStrokeStyle(*context.app, property, "none", "Change stroke marker");
    }
    for (const std::string& id : inspectorSnapshot_.markerIds) {
      const std::string reference = "url(#" + id + ")";
      if (ImGui::Selectable(id.c_str(), current == reference) && context.canMutate) {
        changed =
            applyStrokeStyle(*context.app, property, reference, "Change stroke marker") || changed;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}

bool SidebarPresenter::submitDashPattern(EditorApp& liveApp, std::string_view pattern) {
  return pattern.size() < strokeDasharrayBuffer_.size() && IsValidStrokeDasharray(pattern) &&
         applyStrokeStyle(liveApp, "stroke-dasharray", pattern, "Change stroke dashes");
}

bool SidebarPresenter::setStrokeDashOffset(EditorApp& liveApp, double value) {
  if (!std::isfinite(value)) {
    return false;
  }
  const RcString cssOffset = Lengthd(value, inspectorSnapshot_.strokeDashoffset.unit).toRcString();
  return liveApp.setStylePropertyOnSelection("stroke-dashoffset", cssOffset);
}

bool SidebarPresenter::applyStrokeStyle(EditorApp& liveApp, std::string_view property,
                                        std::string_view value, std::string_view undoLabel) {
  if (!liveApp.hasSelection() || !liveApp.document().hasDocument()) {
    return false;
  }
  const std::string before(liveApp.document().document().source());
  const bool queued = liveApp.setStylePropertyOnSelection(property, value);
  if (queued) {
    liveApp.recordDocumentSourceUndoOnNextFlush(std::string(undoLabel),
                                                liveApp.document().document().svgElement(), before,
                                                /*preserveSelection=*/true);
  }
  return queued;
}

void SidebarPresenter::beginStrokeScalarEdit(EditorApp& liveApp, StrokeScalarField field) {
  if (strokeScalarEdit_.has_value() && strokeScalarEdit_->field == field) {
    return;
  }
  if (strokeScalarEdit_.has_value()) {
    finishStrokeScalarEdit(liveApp, strokeScalarEdit_->field);
  }
  strokeScalarEdit_ = StrokeScalarEdit{
      .field = field,
      .beforeSource = std::string(liveApp.document().document().source()),
  };
}

void SidebarPresenter::trackStrokeScalarItem(const StrokeRenderContext& context,
                                             StrokeScalarField field) {
  if (context.canMutate && ImGui::IsItemActivated()) {
    beginStrokeScalarEdit(*context.app, field);
  }
  if (ImGui::IsItemDeactivated() && strokeScalarEdit_.has_value() &&
      strokeScalarEdit_->field == field) {
    if (context.app != nullptr) {
      finishStrokeScalarEdit(*context.app, field);
    } else {
      strokeScalarEdit_->pendingCommit = true;
    }
  }
}

void SidebarPresenter::finishStrokeScalarEdit(EditorApp& liveApp, StrokeScalarField field) {
  if (!strokeScalarEdit_.has_value() || strokeScalarEdit_->field != field) {
    return;
  }
  StrokeScalarEdit completed = std::move(*strokeScalarEdit_);
  strokeScalarEdit_.reset();
  if (!completed.changed) {
    return;
  }
  const char* label = field == StrokeScalarField::Width        ? "Change stroke width"
                      : field == StrokeScalarField::MiterLimit ? "Change miter limit"
                                                               : "Change dash offset";
  liveApp.recordDocumentSourceUndoOnNextFlush(label, liveApp.document().document().svgElement(),
                                              std::move(completed.beforeSource),
                                              /*preserveSelection=*/true);
}

bool SidebarPresenter::renderTransformPanel(EditorApp* liveApp) {
  if (!inspectorSnapshot_.transform.has_value()) {
    return false;
  }

  bool queuedMutation = false;
  transformFieldRects_.fill(std::nullopt);
  matrixFieldRects_.fill(std::nullopt);

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
  const bool visuallyEditable =
      liveApp != nullptr ? liveEditable : inspectorSnapshot_.transformEditable;
  const bool decomposable = snapshotDecomposed.has_value();

  ImGui::Separator();
  ImGui::TextUnformatted("Transform");

  const bool canEditPosition = visuallyEditable && decomposable && bounds.has_value();
  const bool canEditWidth =
      canEditPosition && static_cast<double>(bounds->width()) > kMinimumSpanForScale;
  const bool canEditHeight =
      canEditPosition && static_cast<double>(bounds->height()) > kMinimumSpanForScale;
  const bool canEditRotation = visuallyEditable && decomposable;

  const float xValue = bounds.has_value() ? static_cast<float>(bounds->topLeft.x) : 0.0f;
  const float yValue = bounds.has_value() ? static_cast<float>(bounds->topLeft.y) : 0.0f;
  const float widthValue = bounds.has_value() ? static_cast<float>(bounds->width()) : 0.0f;
  const float heightValue = bounds.has_value() ? static_cast<float>(bounds->height()) : 0.0f;
  const float rotationValue = decomposable
                                  ? static_cast<float>(snapshotDecomposed->rotationRadians *
                                                       MathConstants<double>::kRadToDeg)
                                  : 0.0f;

  const EditorTheme& theme = EditorTheme::Active();
  constexpr ImGuiTableFlags kTransformTableFlags = ImGuiTableFlags_SizingStretchProp |
                                                   ImGuiTableFlags_PadOuterX |
                                                   ImGuiTableFlags_NoSavedSettings;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(theme.space2, theme.space1));
  if (ImGui::BeginTable("##transform_fields", 5, kTransformTableFlags)) {
    ImGui::TableSetupColumn("##transform_group", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("##transform_axis_first", ImGuiTableColumnFlags_WidthFixed, 18.0f);
    ImGui::TableSetupColumn("##transform_value_first", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("##transform_axis_second", ImGuiTableColumnFlags_WidthFixed, 18.0f);
    ImGui::TableSetupColumn("##transform_value_second", ImGuiTableColumnFlags_WidthStretch, 1.0f);

    const auto renderGroupLabel = [&](const char* label) {
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "%s", label);
    };
    const auto renderField = [&](int axisColumn, int valueColumn, const char* axisLabel,
                                 TransformField field, const char* id, float value, bool canEdit,
                                 const char* undoLabel, float speed, const char* format) {
      ImGui::TableSetColumnIndex(axisColumn);
      if (axisLabel[0] != '\0') {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "%s", axisLabel);
      }
      ImGui::TableSetColumnIndex(valueColumn);
      queuedMutation =
          renderTransformFieldDrag(liveApp, field, id, value, canEdit, undoLabel, speed, format) ||
          queuedMutation;
      const ImVec2 min = ImGui::GetItemRectMin();
      const ImVec2 max = ImGui::GetItemRectMax();
      transformFieldRects_[static_cast<std::size_t>(field)] =
          Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
    };

    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + theme.space1);
    renderGroupLabel("Position");
    renderField(1, 2, "X", TransformField::PositionX, "##transform_x", xValue, canEditPosition,
                "Move element", 0.1f, "%.2f");
    renderField(3, 4, "Y", TransformField::PositionY, "##transform_y", yValue, canEditPosition,
                "Move element", 0.1f, "%.2f");

    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + theme.space1);
    renderGroupLabel("Size");
    renderField(1, 2, "W", TransformField::Width, "##transform_w", widthValue, canEditWidth,
                "Resize element", 0.1f, "%.2f");
    renderField(3, 4, "H", TransformField::Height, "##transform_h", heightValue, canEditHeight,
                "Resize element", 0.1f, "%.2f");

    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + theme.space1);
    renderGroupLabel("Rotation");
    renderField(1, 2, "", TransformField::Rotation, "##transform_r", rotationValue, canEditRotation,
                "Rotate element", 0.1f, "%.1f deg");
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  if (!decomposable) {
    ImGui::TextDisabled("Matrix has skew; edit the raw values below.");
  }

  // Raw matrix disclosure: collapsed by default, always editable so skewed
  // and otherwise non-decomposable matrices keep an editing route.
  if (ImGui::TreeNode("Matrix##transform_matrix")) {
    static constexpr std::array<const char*, 6> kMatrixLabels = {
        "a##transform_mat0", "b##transform_mat1", "c##transform_mat2",
        "d##transform_mat3", "e##transform_mat4", "f##transform_mat5"};
    if (ImGui::BeginTable("##transform_matrix_fields", 4, kTransformTableFlags)) {
      ImGui::TableSetupColumn("##transform_matrix_axis_first", ImGuiTableColumnFlags_WidthFixed,
                              18.0f);
      ImGui::TableSetupColumn("##transform_matrix_value_first", ImGuiTableColumnFlags_WidthStretch,
                              1.0f);
      ImGui::TableSetupColumn("##transform_matrix_axis_second", ImGuiTableColumnFlags_WidthFixed,
                              18.0f);
      ImGui::TableSetupColumn("##transform_matrix_value_second", ImGuiTableColumnFlags_WidthStretch,
                              1.0f);
      for (int i = 0; i < 6; ++i) {
        const bool editingThisCell =
            transformEdit_.has_value() && transformEdit_->field == TransformField::Matrix &&
            transformEdit_->matrixIndex == i && !transformEdit_->pendingCommit;
        double value =
            editingThisCell ? transformEdit_->matrixValues[i] : snapshotTransform.data[i];

        if ((i % 2) == 0) {
          ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + theme.space1);
        }
        const int axisColumn = (i % 2) * 2;
        ImGui::TableSetColumnIndex(axisColumn);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "%c", 'a' + i);
        ImGui::TableSetColumnIndex(axisColumn + 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (!visuallyEditable) {
          ImGui::BeginDisabled();
        }
        const bool changed = ImGui::DragScalar(kMatrixLabels[i], ImGuiDataType_Double, &value,
                                               0.01f, nullptr, nullptr, "%.6g");
        if (!visuallyEditable) {
          ImGui::EndDisabled();
        }
        const ImVec2 cellMin = ImGui::GetItemRectMin();
        const ImVec2 cellMax = ImGui::GetItemRectMax();
        matrixFieldRects_[i] =
            Box2d(Vector2d(cellMin.x, cellMin.y), Vector2d(cellMax.x, cellMax.y));

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
      ImGui::EndTable();
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

  ImGui::SetNextItemWidth(-FLT_MIN);
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

  if (!element.isa<svg::SVGGraphicsElement>()) {
    return;
  }

  TransformEditState state{
      .element = element, .field = field, .matrixIndex = matrixIndex, .undoLabel = undoLabel};
  // `transform()` and `worldBounds()` may materialize lazy layout state and
  // therefore acquire document write access. Do not wrap these DOM getters in
  // a read scope: ConcurrentDom uses a non-recursive lock, so upgrading that
  // scope would deadlock on field activation.
  state.startTransform = element.cast<svg::SVGGraphicsElement>().transform();
  if (element.isa<svg::SVGGeometryElement>()) {
    state.startBounds = element.cast<svg::SVGGeometryElement>().worldBounds();
  }
  // Verbatim attribute bytes so undo restores the user's original source
  // text instead of the canonical serializer output.
  state.sourceTransformAttributeValue = element.getAttribute("transform");
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
  // Carry the author's original transform bytes on the forward ("after")
  // snapshot too so redo re-derives the same syntax-preserving writeback the
  // original edit produced (restore stays false: this is a forward apply).
  UndoSnapshot after{.element = state.element,
                     .transform = state.currentTransform,
                     .writebackTarget = state.writebackTarget,
                     .sourceTransformAttributeValue = state.sourceTransformAttributeValue};
  liveApp.undoTimeline().record(state.undoLabel, std::move(before), std::move(after));

  // Keep the source pane's transform= attribute in lock-step with the DOM;
  // DocumentSyncController drains this queue once per frame. The original
  // attribute bytes let the writeback preserve the author's function syntax
  // (rotate()/translate()/scale()) instead of canonicalizing to matrix().
  if (state.writebackTarget.has_value()) {
    liveApp.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
        .target = *state.writebackTarget,
        .transform = state.currentTransform,
        .sourceTransformAttributeValue = state.sourceTransformAttributeValue,
    });
  }
}

}  // namespace donner::editor
