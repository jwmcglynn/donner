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

#include "donner/base/FormatNumber.h"
#include "donner/base/MathUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/css/CSS.h"
#include "donner/editor/AttachedNumericStepper.h"
#include "donner/editor/DisclosureChevron.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"
#include "donner/editor/InspectorNumericInput.h"
#include "donner/editor/LockState.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/StrokeMarkerPrefabs.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "embed_resources/BootstrapIcons.h"
#include "embed_resources/EditorIcons.h"

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

bool HasPaintedInspectorBounds(const svg::SVGElement& element) {
  return element.isa<svg::SVGTextElement>() || element.isa<svg::SVGGElement>();
}

/// Document-space bounds behind the Inspector's Position and Size fields. Path
/// geometry reports its fill bounds. `<text>` and `<g>` have no path geometry of
/// their own, so they report the box their selection chrome draws: the text's
/// laid-out frame, or the union of the group's rendered children. Those boxes
/// include visible stroke, so a group around a stroked shape reports a larger
/// box than the shape selected alone. Other elements have no bounds, and their
/// Position and Size fields stay disabled.
std::optional<Box2d> InspectorBounds(const svg::SVGElement& element) {
  if (element.isa<svg::SVGGeometryElement>()) {
    return element.cast<svg::SVGGeometryElement>().worldBounds();
  }
  if (HasPaintedInspectorBounds(element)) {
    const std::vector<Box2d> bounds =
        SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(&element, 1));
    if (!bounds.empty()) {
      return bounds.front();
    }
  }
  return std::nullopt;
}

/// Below this document-space span (in user units), a bounds axis is treated
/// as degenerate and the matching Width / Height field is disabled - a scale
/// factor against a ~zero span would explode.
constexpr double kMinimumSpanForScale = 1e-6;

bool IsFiniteTransform(const Transform2d& parentFromEntity) {
  return std::ranges::all_of(parentFromEntity.data, [](double v) { return std::isfinite(v); });
}

/// Maps the selected element's parent coordinates to document coordinates.
Transform2d DocumentFromParentTransform(const svg::SVGElement& element) {
  const std::optional<svg::SVGElement> parent = element.parentElement();
  if (!parent.has_value() || !parent->isa<svg::SVGGraphicsElement>()) {
    return Transform2d();
  }
  return parent->cast<svg::SVGGraphicsElement>().elementFromWorld();
}

/// Document-space translation that moves @p box's top-left edge on one axis to @p value.
std::optional<Transform2d> PositionChange(const std::optional<Box2d>& box, double value,
                                          bool horizontal) {
  if (!box.has_value()) {
    return std::nullopt;
  }
  return horizontal ? Transform2d::Translate(value - box->topLeft.x, 0.0)
                    : Transform2d::Translate(0.0, value - box->topLeft.y);
}

/// Document-space scale about @p box's top-left corner that gives it @p value extent on one axis.
std::optional<Transform2d> SizeChange(const std::optional<Box2d>& box, double value,
                                      bool horizontal) {
  if (!box.has_value()) {
    return std::nullopt;
  }
  const double span = horizontal ? box->width() : box->height();
  if (!(span > kMinimumSpanForScale) || !(value > kMinimumSpanForScale)) {
    return std::nullopt;
  }
  const double factor = value / span;
  return Transform2d::Translate(-box->topLeft) *
         Transform2d::Scale(horizontal ? factor : 1.0, horizontal ? 1.0 : factor) *
         Transform2d::Translate(box->topLeft);
}

struct PaintedSizeSearch {
  double lower = 1e-8;
  double upper = 1.0;
  double lowerExtent = 0.0;
  double upperExtent = 0.0;
  double factor = 1.0;
  std::optional<Box2d> bounds;
};

using BoundsAtScale = std::function<std::optional<Box2d>(double)>;
using BoundsExtent = std::function<double(const Box2d&)>;

bool BracketPaintedSize(PaintedSizeSearch& search, double value, const BoundsAtScale& scaledBounds,
                        const BoundsExtent& extent) {
  const auto minimum = scaledBounds(search.lower);
  if (!minimum.has_value() || !std::isfinite(extent(*minimum)) || extent(*minimum) > value) {
    return false;
  }
  search.bounds = scaledBounds(search.upper);
  for (int i = 0; i < 32 && search.bounds.has_value() && extent(*search.bounds) < value; ++i) {
    search.upper *= 2.0;
    search.bounds = scaledBounds(search.upper);
  }
  if (!search.bounds.has_value() || !std::isfinite(extent(*search.bounds)) ||
      extent(*search.bounds) < value) {
    return false;
  }
  search.lowerExtent = extent(*minimum);
  search.upperExtent = extent(*search.bounds);
  search.factor = search.upper;
  return true;
}

bool RefinePaintedSize(PaintedSizeSearch& search, double value, const BoundsAtScale& scaledBounds,
                       const BoundsExtent& extent) {
  const double tolerance = 1e-10 * std::max(1.0, value);
  for (int i = 0; i < 64 && std::abs(extent(*search.bounds) - value) > tolerance; ++i) {
    // Interpolation solves fixed-padding rectangular bounds directly. Safeguard curved or
    // changing joins against a stalled endpoint, retaining a bounded shrinking interval.
    const double fraction =
        (value - search.lowerExtent) / (search.upperExtent - search.lowerExtent);
    search.factor = search.lower + (search.upper - search.lower) * fraction;
    if (!(search.factor > search.lower && search.factor < search.upper)) {
      search.factor = (search.lower + search.upper) * 0.5;
    }
    search.bounds = scaledBounds(search.factor);
    if (!search.bounds.has_value() || !std::isfinite(extent(*search.bounds))) {
      return false;
    }
    if (extent(*search.bounds) < value) {
      search.lower = search.factor;
      search.lowerExtent = extent(*search.bounds);
    } else {
      search.upper = search.factor;
      search.upperExtent = extent(*search.bounds);
    }
  }
  return true;
}

Transform2d AxisScale(double factor, bool horizontal) {
  return Transform2d::Scale(horizontal ? factor : 1.0, horizontal ? 1.0 : factor);
}

/// Solve against immutable painted geometry, then move the painted origin back to its baseline.
std::optional<Transform2d> PaintedSizeChange(
    const Box2d& start, double value, bool horizontal,
    const std::function<std::optional<Box2d>(const Transform2d&)>& evaluate,
    std::size_t* evaluationCount) {
  if (!(value > kMinimumSpanForScale) || !std::isfinite(value)) {
    return std::nullopt;
  }
  *evaluationCount = 0;
  const BoundsAtScale scaledBounds = [&](double factor) {
    ++*evaluationCount;
    return evaluate(AxisScale(factor, horizontal));
  };
  const BoundsExtent extent = [&](const Box2d& bounds) {
    return horizontal ? bounds.width() : bounds.height();
  };
  PaintedSizeSearch search;
  if (!BracketPaintedSize(search, value, scaledBounds, extent) ||
      !RefinePaintedSize(search, value, scaledBounds, extent)) {
    return std::nullopt;
  }
  // Join topology can change during a scale. Accept only an actually attained extent;
  // a failed solve never changes the live transform.
  if (std::abs(extent(*search.bounds) - value) > 1e-7 * std::max(1.0, value)) {
    return std::nullopt;
  }
  return AxisScale(search.factor, horizontal) *
         Transform2d::Translate(start.topLeft - search.bounds->topLeft);
}

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
    if (std::string_view(tag) == "marker" && !StrokeMarkerPrefabForElement(element).has_value()) {
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

constexpr int kStrokePreviewRasterSizePx = 96;
constexpr int kStrokeWidthPresetRasterSizePx = 256;

const std::optional<svg::RendererBitmap>& CachedStrokePreviewBitmap(StrokePreviewIcon icon) {
  static const std::array<std::optional<svg::RendererBitmap>, kStrokePreviewIcons.size()> bitmaps =
      [] {
        std::array<std::optional<svg::RendererBitmap>, kStrokePreviewIcons.size()> result;
        for (std::size_t index = 0; index < kStrokePreviewIcons.size(); ++index) {
          result[index] = RenderEmbeddedSvgArtwork(StrokePreviewIconSvg(kStrokePreviewIcons[index]),
                                                   kStrokePreviewRasterSizePx);
        }
        return result;
      }();
  return bitmaps[static_cast<std::size_t>(icon)];
}

const std::optional<svg::RendererBitmap>& CachedStrokeWidthPresetBitmap() {
  static const std::optional<svg::RendererBitmap> bitmap =
      RenderEmbeddedSvgArtwork(StrokeWidthPresetSvg(), kStrokeWidthPresetRasterSizePx);
  return bitmap;
}

bool RenderStrokeChoice(const char* id, const char* tooltip, StrokePreviewIcon icon, bool selected,
                        const EditorTheme& theme,
                        const SidebarPresenter::IconTextureProvider& iconTextureProvider,
                        std::optional<Box2d>* rect) {
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
  if (iconTextureProvider) {
    const std::optional<svg::RendererBitmap>& bitmap = CachedStrokePreviewBitmap(icon);
    if (bitmap.has_value()) {
      const SidebarPresenter::IconTexture texture =
          iconTextureProvider(StrokePreviewIconTextureKey(icon), *bitmap);
      if (texture.texture != 0) {
        const ImVec2 iconMin(min.x + 4.0f, min.y + 2.0f);
        const ImVec2 iconMax(iconMin.x + 24.0f, iconMin.y + 24.0f);
        const ImVec2 uvMax(static_cast<float>(texture.uvBottomRight.x),
                           static_cast<float>(texture.uvBottomRight.y));
        draw->AddImage(texture.texture, iconMin, iconMax, ImVec2(0.0f, 0.0f), uvMax,
                       IM_COL32(255, 255, 255, 255));
      }
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }
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

}  // namespace

std::span<const unsigned char> StrokeWidthPresetSvg() {
  return embedded::kStrokeWidthPresetsSvg;
}

std::uint64_t StrokeWidthPresetTextureKey() {
  return 0xf800000000000008ull;
}

std::span<const unsigned char> StrokePreviewIconSvg(StrokePreviewIcon icon) {
  switch (icon) {
    case StrokePreviewIcon::ButtCap: return embedded::kStrokeCapButtSvg;
    case StrokePreviewIcon::RoundCap: return embedded::kStrokeCapRoundSvg;
    case StrokePreviewIcon::SquareCap: return embedded::kStrokeCapSquareSvg;
    case StrokePreviewIcon::MiterJoin: return embedded::kStrokeJoinMiterSvg;
    case StrokePreviewIcon::RoundJoin: return embedded::kStrokeJoinRoundSvg;
    case StrokePreviewIcon::BevelJoin: return embedded::kStrokeJoinBevelSvg;
  }
  return embedded::kStrokeCapButtSvg;
}

std::uint64_t StrokePreviewIconTextureKey(StrokePreviewIcon icon) {
  constexpr std::uint64_t kIconTextureKeyBase = 0xf800000000000000ull;
  return kIconTextureKeyBase + static_cast<std::uint64_t>(icon) + 1u;
}

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
  static const std::array<EmbeddedSvgIconRequest, 12> kRequests = {{
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
      {StrokePreviewIconSvg(StrokePreviewIcon::ButtCap), kStrokePreviewRasterSizePx,
       /*tintableMask=*/false},
      {StrokePreviewIconSvg(StrokePreviewIcon::RoundCap), kStrokePreviewRasterSizePx,
       /*tintableMask=*/false},
      {StrokePreviewIconSvg(StrokePreviewIcon::SquareCap), kStrokePreviewRasterSizePx,
       /*tintableMask=*/false},
      {StrokePreviewIconSvg(StrokePreviewIcon::MiterJoin), kStrokePreviewRasterSizePx,
       /*tintableMask=*/false},
      {StrokePreviewIconSvg(StrokePreviewIcon::RoundJoin), kStrokePreviewRasterSizePx,
       /*tintableMask=*/false},
      {StrokePreviewIconSvg(StrokePreviewIcon::BevelJoin), kStrokePreviewRasterSizePx,
       /*tintableMask=*/false},
      {StrokeWidthPresetSvg(), kStrokeWidthPresetRasterSizePx, /*tintableMask=*/false},
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

std::optional<Box2d> SidebarPresenter::cachedInspectorBounds(const EditorApp& app,
                                                             const svg::SVGElement& element) {
  const std::uint64_t documentGeneration = app.document().documentGeneration();
  const std::uint64_t frameVersion = app.document().currentFrameVersion();
  // Canvas commits change layout without an editor frame version bump, but do
  // advance the DOM revision.
  const std::uint64_t domRevision = app.document().document().handle()->revision();
  if (!inspectorBoundsCache_.has_value() || inspectorBoundsCache_->element != element ||
      inspectorBoundsCache_->documentGeneration != documentGeneration ||
      inspectorBoundsCache_->frameVersion != frameVersion ||
      inspectorBoundsCache_->domRevision != domRevision) {
    inspectorBoundsCache_ = InspectorBoundsCache{.element = element,
                                                 .documentGeneration = documentGeneration,
                                                 .frameVersion = frameVersion,
                                                 .domRevision = domRevision,
                                                 .bounds = InspectorBounds(element)};
  }
  return inspectorBoundsCache_->bounds;
}

void SidebarPresenter::refreshSnapshot(const EditorApp& app) {
  discardForeignTransformEdit(app);
  discardForeignStrokeEdits(app);
  if (!app.hasDocument()) {
    treeSnapshot_.reset();
    inspectorSnapshot_ = InspectorSnapshot{};
    inspectorBoundsCache_.reset();
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
  inspector.document = app.document().document().handle();
  inspector.documentGeneration = app.document().documentGeneration();
  inspector.selection = selectionList;
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

    inspector.bounds = cachedInspectorBounds(app, selected);
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

namespace {

/// Whether \p unit resolves against the document's user coordinate system.
bool IsDocumentSizeRelative(LengthUnit unit) {
  return unit == LengthUnit::Percent || unit == LengthUnit::Vw || unit == LengthUnit::Vh ||
         unit == LengthUnit::Vmin || unit == LengthUnit::Vmax;
}

/// Tooltip for a transform field whose typed text is rejected.
const char* TransformFieldHint(SidebarPresenter::TransformField field, int matrixIndex) {
  if (field == SidebarPresenter::TransformField::Rotation) {
    return kInspectorDegreesHint;
  }
  return field == SidebarPresenter::TransformField::Matrix && matrixIndex < 4
             ? kInspectorNumberHint
             : kInspectorLengthHint;
}

/// The document's user coordinate system, the reference for typed percentages: its viewBox, or
/// the root's absolute width and height. Empty when neither is known.
Box2d PercentReference(svg::SVGDocument& document) {
  const svg::DocumentReadAccess access = document.readAccess();
  const svg::SVGSVGElement root = document.svgElement();
  if (const std::optional<Box2d> viewBox = root.viewBox()) {
    return *viewBox;
  }
  const std::optional<Lengthd> width = root.width();
  const std::optional<Lengthd> height = root.height();
  if (!width.has_value() || !height.has_value() || !width->isAbsoluteSize() ||
      !height->isAbsoluteSize()) {
    return Box2d();
  }
  return Box2d::FromXYWH(0.0, 0.0, width->toPixels(Box2d(), FontMetrics()),
                         height->toPixels(Box2d(), FontMetrics()));
}

/// Font sizes for em, ex and rem typed into a transform field of \p element.
FontMetrics TypedUnitFontMetrics(const svg::SVGSVGElement& root, const svg::SVGElement& element) {
  FontMetrics metrics;
  metrics.fontSize = element.getComputedStyle().fontSize.get().value().toPixels(
      Box2d(), FontMetrics(), Lengthd::Extent::Mixed);
  metrics.rootFontSize = root.getComputedStyle().fontSize.get().value().toPixels(
      Box2d(), FontMetrics(), Lengthd::Extent::Mixed);
  return metrics;
}

}  // namespace

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
      queuedMutation = renderStrokeControlsPanel(liveApp, iconTextureProvider);
      queuedMutation =
          RenderPathOperationsPanel(liveApp, inspectorSnapshot_.pathOperationAvailability,
                                    iconTextureProvider) ||
          queuedMutation;
    } else {
      ImGui::TextDisabled("Select a single element to inspect attributes.");
      queuedMutation = renderStrokeControlsPanel(liveApp, iconTextureProvider);
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
    queuedMutation = renderStrokeControlsPanel(liveApp, iconTextureProvider) || queuedMutation;
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

bool SidebarPresenter::renderStrokeControlsPanel(EditorApp* liveApp,
                                                 const IconTextureProvider& iconTextureProvider) {
  const EditorTheme& theme = EditorTheme::Active();
  ImGui::Separator();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.textMuted), "STROKE");
  // A deactivated input can still return its previous text after ClearActiveID(). Settle its
  // original transaction first, then keep that retiring frame from mutating the new selection.
  const bool changingEditSelection = strokeSelectionChanged(liveApp);
  const bool restoredCancelledEdit = settlePendingStrokeEdit(liveApp);
  const bool canMutate = liveApp != nullptr && liveApp->hasSelection() &&
                         inspectorSnapshot_.strokeEditable && !changingEditSelection;
  ImGui::BeginDisabled(!inspectorSnapshot_.strokeEditable || strokeEditSettlementPending());
  const StrokeRenderContext context{liveApp, theme, ImGui::GetCursorPosX(), canMutate,
                                    iconTextureProvider};
  bool queuedMutation = renderStrokeWidthRow(context) || restoredCancelledEdit;
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
  const bool fieldActivated = ImGui::IsItemActivated();
  // Enter or Escape ends typing; the presets must not linger over an unfocused field.
  const bool fieldDismissed =
      ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_Escape, false));
  queuedMutation = renderStrokeWidthStepper(context, widthLength, width) || queuedMutation;
  queuedMutation =
      renderStrokeWidthPresetPopup(context, widthLength, fieldActivated, fieldDismissed) ||
      queuedMutation;
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeWidthField(const StrokeRenderContext& context,
                                              const Lengthd& widthLength, float* width) {
  ImGui::SetNextItemWidth(96.0f);
  const InspectorLengthEdit edit =
      InputInspectorLength("##stroke_width", widthLength,
                           strokeLengthBareUnit(StrokeScalarField::Width, widthLength.unit),
                           /*allowNegative=*/false, context.theme);
  {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    strokeWidthRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  }
  bool queuedMutation = beginStrokeLengthEdit(context, StrokeScalarField::Width, widthLength.unit);
  if (edit.typed.has_value() && context.canMutate) {
    *width = static_cast<float>(edit.typed->value);
    queuedMutation = applyStrokeWidth(*context.app, *edit.typed) || queuedMutation;
  }
  queuedMutation =
      restoreStrokeLengthOnCancel(context, StrokeScalarField::Width, edit) || queuedMutation;
  trackStrokeScalarItem(context, StrokeScalarField::Width);
  if (!edit.rejected && ImGui::IsItemHovered()) {
    ImGui::SetTooltip(widthLength.unit == LengthUnit::None ? "Stroke width in user units"
                                                           : "Stroke width retains its SVG unit");
  }
  return queuedMutation;
}

bool SidebarPresenter::applyStrokeWidth(EditorApp& liveApp, Lengthd width) {
  beginStrokeScalarEdit(liveApp, StrokeScalarField::Width);
  if (width.unit == LengthUnit::None || width.unit == LengthUnit::Px) {
    width.value = std::min(width.value, 200.0);
    liveApp.setActiveStrokeWidth(static_cast<float>(width.value));
  }
  const bool changed = liveApp.setStylePropertyOnSelection("stroke-width", width.toRcString());
  strokeScalarEdit_->changed |= changed;
  return changed;
}

bool SidebarPresenter::restoreStrokeLengthOnCancel(const StrokeRenderContext& context,
                                                   StrokeScalarField field,
                                                   const InspectorLengthEdit& edit) {
  noteStrokeLengthText(field, edit);
  if (!ImGui::IsItemDeactivated() || !strokeLengthEditStart_.has_value() ||
      strokeLengthEditStart_->field != field) {
    return false;
  }
  if (!strokeLengthEditStart_->lastTextRejected && !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    strokeLengthEditStart_.reset();
    return false;
  }
  strokeLengthEditStart_->pendingCancel = true;
  return finishPendingStrokeCancel(context.app);
}

void SidebarPresenter::noteStrokeLengthText(StrokeScalarField field,
                                            const InspectorLengthEdit& edit) {
  // Another input activating first replaces ImGui's text state, so keep the last verdict.
  if (edit.typing && strokeLengthEditStart_.has_value() && strokeLengthEditStart_->field == field) {
    strokeLengthEditStart_->lastTextRejected = edit.rejected;
  }
}

bool SidebarPresenter::settlePendingStrokeEdit(EditorApp* liveApp) {
  bool restored = false;
  if (liveApp != nullptr) {
    discardForeignStrokeEdits(*liveApp);
    restored = settleStrokeSelectionChange(*liveApp);
  }
  restored = finishPendingStrokeCancel(liveApp) || restored;
  if (liveApp != nullptr && strokeScalarEdit_.has_value() && strokeScalarEdit_->pendingCommit) {
    finishStrokeScalarEdit(*liveApp, strokeScalarEdit_->field);
  }
  return restored;
}

bool SidebarPresenter::strokeCancellationPending() const {
  return strokeLengthEditStart_.has_value() && strokeLengthEditStart_->pendingCancel;
}

bool SidebarPresenter::finishPendingStrokeCancel(EditorApp* liveApp) {
  if (liveApp == nullptr || !strokeCancellationPending()) {
    return false;
  }
  const StrokeLengthEditStart start = std::move(*strokeLengthEditStart_);
  strokeLengthEditStart_.reset();
  if (!strokeScalarEdit_.has_value() || strokeScalarEdit_->field != start.field ||
      !strokeScalarEdit_->changed) {
    return false;
  }
  return restoreStrokeStyles(*liveApp, start);
}

bool SidebarPresenter::beginStrokeLengthEdit(const StrokeRenderContext& context,
                                             StrokeScalarField field, LengthUnit unit) {
  if (strokeEditSettlementPending()) {
    return false;
  }
  bool restored = false;
  if (ImGui::IsItemActivated()) {
    StrokeLengthEditStart next{.owner = strokeEditOwner(context.app),
                               .field = field,
                               .selection = inspectorSnapshot_.selection,
                               .unit = unit,
                               .styles = {},
                               .activeWidth = {}};
    if (!prepareStrokeScalarEdit(context.app, field, &next)) {
      ImGui::ClearActiveID();
      return false;
    }
    restored = !next.styles.empty();
    strokeLengthEditStart_ = std::move(next);
  }
  captureStrokeLengthBaseline(context, field);
  return restored;
}

void SidebarPresenter::captureStrokeLengthBaseline(const StrokeRenderContext& context,
                                                   StrokeScalarField field) {
  if (context.canMutate && strokeLengthEditStart_.has_value() &&
      strokeLengthEditStart_->field == field && strokeLengthEditStart_->styles.empty()) {
    StrokeLengthEditStart& start = *strokeLengthEditStart_;
    start.activeWidth = context.app->activePaintStyle().strokeWidth;
    for (const svg::SVGElement& element : context.app->selectedElements()) {
      start.styles.emplace_back(element, element.getAttribute("style"));
    }
  }
}

bool SidebarPresenter::strokeEditSettlementPending() const {
  return strokeCancellationPending() ||
         (strokeScalarEdit_.has_value() && strokeScalarEdit_->pendingCommit);
}

bool SidebarPresenter::strokeSelectionChanged(const EditorApp* app) const {
  return app != nullptr && ((strokeLengthEditStart_.has_value() &&
                             strokeLengthEditStart_->selection != app->selectedElements()) ||
                            (strokeScalarEdit_.has_value() &&
                             strokeScalarEdit_->selection != app->selectedElements()));
}

bool SidebarPresenter::settleStrokeSelectionChange(EditorApp& app) {
  if (!strokeSelectionChanged(&app)) {
    return false;
  }
  const ImGuiID itemId = strokeLengthEditStart_.has_value() ? strokeLengthEditStart_->owner.itemId
                                                            : strokeScalarEdit_->owner.itemId;
  const bool restored = settleStrokeLengthEdit(app);
  if (strokeScalarEdit_.has_value()) {
    finishStrokeScalarEdit(app, strokeScalarEdit_->field);
  }
  if (ImGui::GetActiveID() == itemId) {
    ImGui::ClearActiveID();
  }
  return restored;
}

bool SidebarPresenter::settleStrokeLengthEdit(EditorApp& app) {
  if (!strokeLengthEditStart_.has_value()) {
    return false;
  }
  if (strokeLengthEditStart_->lastTextRejected || strokeLengthEditStart_->pendingCancel) {
    strokeLengthEditStart_->pendingCancel = true;
    return finishPendingStrokeCancel(&app);
  }
  strokeLengthEditStart_.reset();
  return false;
}

std::optional<SidebarPresenter::StrokeScalarField> SidebarPresenter::activeStrokeField() const {
  if (strokeLengthEditStart_.has_value()) {
    return strokeLengthEditStart_->field;
  }
  if (strokeScalarEdit_.has_value()) {
    return strokeScalarEdit_->field;
  }
  return std::nullopt;
}

bool SidebarPresenter::prepareStrokeScalarEdit(EditorApp* app, StrokeScalarField field,
                                               StrokeLengthEditStart* nextLength) {
  const std::optional<StrokeScalarField> previous = activeStrokeField();
  if (previous.has_value() && *previous != field) {
    if (app == nullptr) {
      return deferStrokeFieldTransition();
    }
    std::optional<std::string> restoredSource = restoreStrokeLengthForTransition(*app, nextLength);
    if (strokeScalarEdit_.has_value()) {
      finishStrokeScalarEdit(*app, strokeScalarEdit_->field);
    }
    startStrokeScalarEdit(*app, field, std::move(restoredSource));
  } else if (app != nullptr && !strokeScalarEdit_.has_value()) {
    startStrokeScalarEdit(*app, field);
  }
  return true;
}

bool SidebarPresenter::deferStrokeFieldTransition() {
  if (strokeLengthEditStart_.has_value()) {
    if (strokeLengthEditStart_->lastTextRejected || strokeLengthEditStart_->pendingCancel) {
      strokeLengthEditStart_->pendingCancel = true;
    } else {
      strokeLengthEditStart_.reset();
    }
  }
  if (strokeScalarEdit_.has_value()) {
    strokeScalarEdit_->pendingCommit = true;
  }
  return !strokeEditSettlementPending();
}

std::optional<std::string> SidebarPresenter::restoreStrokeLengthForTransition(
    EditorApp& app, StrokeLengthEditStart* nextLength) {
  if (!strokeLengthEditStart_.has_value()) {
    return std::nullopt;
  }
  const StrokeLengthEditStart previous = *strokeLengthEditStart_;
  if (!settleStrokeLengthEdit(app)) {
    return std::nullopt;
  }
  // Rollback is queued until the frame flush. The next edit starts from the restored state,
  // not the still-visible intermediate DOM/source, so its own cancellation and undo stay separate.
  if (nextLength != nullptr) {
    nextLength->styles = previous.styles;
    nextLength->activeWidth = previous.activeWidth;
  }
  return strokeScalarEdit_->beforeSource;
}

void SidebarPresenter::startStrokeScalarEdit(EditorApp& app, StrokeScalarField field,
                                             std::optional<std::string> beforeSource) {
  strokeScalarEdit_ = StrokeScalarEdit{
      .owner = strokeEditOwner(&app),
      .field = field,
      .selection = app.selectedElements(),
      .beforeSource = beforeSource.has_value() ? std::move(*beforeSource)
                                               : std::string(app.document().document().source()),
  };
}

bool SidebarPresenter::restoreStrokeStyles(EditorApp& liveApp, const StrokeLengthEditStart& start) {
  for (const auto& [element, style] : start.styles) {
    liveApp.applyMutation(
        style.has_value()
            ? EditorCommand::SetAttributeCommand(element, "style", std::string(*style))
            : EditorCommand::RemoveAttributeCommand(element, "style"));
  }
  if (start.styles.empty()) {
    return false;
  }
  // Typing set the width for new shapes as a plain number, so only such a width can be restored.
  const LengthUnit activeUnit = start.activeWidth.unit;
  if (activeUnit == LengthUnit::None || activeUnit == LengthUnit::Px) {
    liveApp.setActiveStrokeWidth(start.activeWidth.value);
  }
  // The selection is back to its state before typing, so the edit records no undo entry.
  strokeScalarEdit_->changed = false;
  return true;
}

LengthUnit SidebarPresenter::strokeLengthBareUnit(StrokeScalarField field, LengthUnit shown) const {
  return strokeLengthEditStart_.has_value() && strokeLengthEditStart_->field == field
             ? strokeLengthEditStart_->unit
             : shown;
}

bool SidebarPresenter::renderStrokeWidthStepper(const StrokeRenderContext& context,
                                                const Lengthd& widthLength, float width) {
  if (!strokeWidthRect_.has_value()) {
    return false;
  }
  const Box2d& field = *strokeWidthRect_;
  const AttachedNumericStepperResult steps = RenderAttachedNumericStepper(
      "stroke_width_steps",
      ImVec2(static_cast<float>(field.topLeft.x), static_cast<float>(field.topLeft.y)),
      ImVec2(static_cast<float>(field.bottomRight.x), static_cast<float>(field.bottomRight.y)),
      context.theme);
  strokeIncrementRect_ = steps.incrementRect;
  strokeDecrementRect_ = steps.decrementRect;
  if (!context.canMutate || (!steps.increment && !steps.decrement)) {
    return false;
  }

  width = std::max(0.0f, width + (steps.increment ? 1.0f : -1.0f));
  if (widthLength.unit == LengthUnit::None || widthLength.unit == LengthUnit::Px) {
    context.app->setActiveStrokeWidth(width);
  }
  const RcString cssWidth = Lengthd(width, widthLength.unit).toRcString();
  return applyStrokeStyle(*context.app, "stroke-width", cssWidth, "Change stroke width");
}

bool SidebarPresenter::renderStrokeWidthPresetPopup(const StrokeRenderContext& context,
                                                    const Lengthd& widthLength, bool fieldActivated,
                                                    bool fieldDismissed) {
  strokeWidthPresetRects_.fill(std::nullopt);
  if (!strokeWidthRect_.has_value()) {
    return false;
  }
  const Box2d& field = *strokeWidthRect_;
  const ImVec2 fieldMin(static_cast<float>(field.topLeft.x), static_cast<float>(field.topLeft.y));
  const ImVec2 fieldMax(static_cast<float>(field.bottomRight.x),
                        static_cast<float>(field.bottomRight.y));
  if (!BeginHybridNumericPresetPopup("##stroke_width_presets", fieldMin, fieldMax,
                                     fieldActivated && context.canMutate, 300.0f)) {
    return false;
  }
  if (fieldDismissed) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return false;
  }

  bool queuedMutation = false;
  const std::optional<svg::RendererBitmap>& bitmap = CachedStrokeWidthPresetBitmap();
  IconTexture texture;
  if (bitmap.has_value() && context.iconTextureProvider) {
    texture = context.iconTextureProvider(StrokeWidthPresetTextureKey(), *bitmap);
  }
  for (std::size_t index = 0; index < kStrokeWidthPresetValues.size(); ++index) {
    queuedMutation =
        renderStrokeWidthPresetRow(context, widthLength, texture, index) || queuedMutation;
  }
  ImGui::EndPopup();
  return queuedMutation;
}

bool SidebarPresenter::renderStrokeWidthPresetRow(const StrokeRenderContext& context,
                                                  const Lengthd& widthLength,
                                                  const IconTexture& texture, std::size_t index) {
  constexpr float kPreviewWidth = 32.0f;
  constexpr float kRowHeight = 32.0f;
  ImGui::PushID(static_cast<int>(index));
  const float preset = kStrokeWidthPresetValues[index];
  const bool selected =
      (widthLength.unit == LengthUnit::None || widthLength.unit == LengthUnit::Px) &&
      std::abs(widthLength.value - preset) < 0.001;
  const bool chosen = ImGui::Selectable("##width_preset", selected, 0, ImVec2(0.0f, kRowHeight));
  const ImVec2 rowMin = ImGui::GetItemRectMin();
  const ImVec2 rowMax = ImGui::GetItemRectMax();
  strokeWidthPresetRects_[index] =
      Box2d(Vector2d(rowMin.x, rowMin.y), Vector2d(rowMax.x, rowMax.y));
  if (texture.texture != 0) {
    const float uScale = static_cast<float>(texture.uvBottomRight.x) / 256.0f;
    const float vScale = static_cast<float>(texture.uvBottomRight.y) / 256.0f;
    ImGui::GetWindowDrawList()->AddImage(
        texture.texture, ImVec2(rowMin.x + 6.0f, rowMin.y),
        ImVec2(rowMin.x + 6.0f + kPreviewWidth, rowMin.y + kRowHeight),
        ImVec2(0.0f, static_cast<float>(index) * 32.0f * vScale),
        ImVec2(kPreviewWidth * uScale, static_cast<float>(index + 1u) * 32.0f * vScale));
  }
  const std::string label = donner::detail::FormatNumberForSVG(preset);
  ImGui::GetWindowDrawList()->AddText(
      ImVec2(rowMin.x + 51.0f, rowMin.y + (kRowHeight - ImGui::GetFontSize()) * 0.5f),
      selected ? context.theme.accentDefault : context.theme.textPrimary, label.c_str());
  bool changed = false;
  if (chosen && context.canMutate) {
    context.app->setActiveStrokeWidth(preset);
    changed = applyStrokeStyle(*context.app, "stroke-width", label, "Choose stroke width");
    ImGui::CloseCurrentPopup();
  }
  ImGui::PopID();
  return changed;
}

bool SidebarPresenter::renderStrokeCapRow(const StrokeRenderContext& context) {
  EditorApp* liveApp = context.app;
  const EditorTheme& theme = context.theme;
  const float rowStartX = context.rowStartX;
  const bool canMutate = context.canMutate;
  bool queuedMutation = false;
  constexpr std::array<const char*, 3> kCaps = {"butt", "round", "square"};
  constexpr std::array<StrokePreviewIcon, 3> kCapIcons = {
      StrokePreviewIcon::ButtCap, StrokePreviewIcon::RoundCap, StrokePreviewIcon::SquareCap};
  const int cap = std::clamp(inspectorSnapshot_.strokeLinecap, 0, 2);
  StrokeRowLabel("Cap", rowStartX, theme);
  ImGui::PushID("stroke_caps");
  for (int index = 0; index < static_cast<int>(kCaps.size()); ++index) {
    if (index > 0) {
      ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::PushID(index);
    if (RenderStrokeChoice("##choice", kCaps[index], kCapIcons[index], cap == index, theme,
                           context.iconTextureProvider, &strokeCapRects_[index]) &&
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
  constexpr std::array<StrokePreviewIcon, 3> kJoinIcons = {
      StrokePreviewIcon::MiterJoin, StrokePreviewIcon::RoundJoin, StrokePreviewIcon::BevelJoin};
  for (std::size_t position = 0; position < kCommonJoins.size(); ++position) {
    const int index = kCommonJoins[position];
    if (position > 0u) {
      ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::PushID(index);
    if (RenderStrokeChoice("##choice", kJoins[index], kJoinIcons[position],
                           join == index || (index == 0 && (join == 1 || join == 4)), theme,
                           context.iconTextureProvider, &strokeJoinRects_[index]) &&
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
  const EditorTheme& theme = context.theme;
  const float rowStartX = context.rowStartX;
  bool queuedMutation = false;
  const int join = std::clamp(inspectorSnapshot_.strokeLinejoin, 0, 4);
  strokeMiterLimitRect_.reset();
  strokeMiterIncrementRect_.reset();
  strokeMiterDecrementRect_.reset();
  if (join == 0 || join == 1 || join == 4) {
    StrokeRowLabel("Limit", rowStartX, theme);
    float miterlimit = inspectorSnapshot_.strokeMiterlimit;
    ImGui::SetNextItemWidth(90.0f);
    const bool limitChanged =
        ImGui::InputFloat("##stroke_miter_limit", &miterlimit, 0.0f, 0.0f, "%.1f");
    queuedMutation =
        renderStrokeMiterInputValue(context, miterlimit, limitChanged) || queuedMutation;
    trackStrokeScalarItem(context, StrokeScalarField::MiterLimit);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool fieldHovered = ImGui::IsItemHovered();
    strokeMiterLimitRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
    queuedMutation =
        renderStrokeMiterStepper(context, *strokeMiterLimitRect_, miterlimit) || queuedMutation;
    if (fieldHovered) {
      ImGui::SetTooltip("Maximum sharp-corner length relative to stroke width");
    }
    if (!strokeMiterError_.empty()) {
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.destructive), "%s",
                         strokeMiterError_.c_str());
    }
  }

  return queuedMutation;
}

bool SidebarPresenter::renderStrokeMiterInputValue(const StrokeRenderContext& context,
                                                   float miterlimit, bool inputChanged) {
  if (!inputChanged || !context.canMutate) {
    return false;
  }
  if (!std::isfinite(miterlimit) || miterlimit < 1.0f) {
    strokeMiterError_ = "Limit must be at least 1";
    return false;
  }
  beginStrokeScalarEdit(*context.app, StrokeScalarField::MiterLimit);
  const bool changed =
      context.app->setStylePropertyOnSelection("stroke-miterlimit", std::to_string(miterlimit));
  strokeScalarEdit_->changed |= changed;
  strokeMiterError_.clear();
  return changed;
}

bool SidebarPresenter::renderStrokeMiterStepper(const StrokeRenderContext& context,
                                                const Box2d& field, float miterlimit) {
  const ImVec2 min(static_cast<float>(field.topLeft.x), static_cast<float>(field.topLeft.y));
  const ImVec2 max(static_cast<float>(field.bottomRight.x),
                   static_cast<float>(field.bottomRight.y));
  const AttachedNumericStepperResult steps =
      RenderAttachedNumericStepper("stroke_miter_steps", min, max, context.theme);
  strokeMiterIncrementRect_ = steps.incrementRect;
  strokeMiterDecrementRect_ = steps.decrementRect;
  if (!context.canMutate || (!steps.increment && !steps.decrement)) {
    return false;
  }
  miterlimit = std::max(1.0f, miterlimit + (steps.increment ? 1.0f : -1.0f));
  const bool changed =
      applyStrokeStyle(*context.app, "stroke-miterlimit",
                       donner::detail::FormatNumberForSVG(miterlimit), "Change stroke miter limit");
  strokeMiterError_.clear();
  return changed;
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
  ImGui::SetNextItemWidth(96.0f);
  const InspectorLengthEdit edit =
      InputInspectorLength("##stroke_dash_offset", offsetLength,
                           strokeLengthBareUnit(StrokeScalarField::DashOffset, offsetLength.unit),
                           /*allowNegative=*/true, context.theme);
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  strokeDashOffsetRect_ = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
  bool queuedMutation =
      beginStrokeLengthEdit(context, StrokeScalarField::DashOffset, offsetLength.unit);
  if (edit.typed.has_value() && context.canMutate) {
    queuedMutation = applyStrokeDashOffset(*context.app, *edit.typed) || queuedMutation;
  }
  queuedMutation =
      restoreStrokeLengthOnCancel(context, StrokeScalarField::DashOffset, edit) || queuedMutation;
  trackStrokeScalarItem(context, StrokeScalarField::DashOffset);
  return renderDashOffsetStepper(context, offsetLength, min, max) || queuedMutation;
}

bool SidebarPresenter::renderDashOffsetStepper(const StrokeRenderContext& context,
                                               const Lengthd& offsetLength, const ImVec2& fieldMin,
                                               const ImVec2& fieldMax) {
  const AttachedNumericStepperResult steps =
      RenderAttachedNumericStepper("stroke_dash_offset_steps", fieldMin, fieldMax, context.theme);
  strokeDashOffsetIncrementRect_ = steps.incrementRect;
  if (!context.canMutate || (!steps.increment && !steps.decrement)) {
    return false;
  }
  // ImGui's numeric step sizes: 0.1 per click, 1 with Ctrl held.
  const double step = (ImGui::GetIO().KeyCtrl ? 1.0 : 0.1) * (steps.increment ? 1.0 : -1.0);
  // Drop the binary floating-point residue that repeated 0.1 steps leave in the written value.
  const double sum = offsetLength.value + step;
  const double stepped = std::abs(sum) < 1e6 ? std::round(sum * 1e9) / 1e9 : sum;
  return applyStrokeStyle(*context.app, "stroke-dashoffset",
                          Lengthd(stepped, offsetLength.unit).toRcString(), "Change dash offset");
}

bool SidebarPresenter::renderStrokeMarkers(const StrokeRenderContext& context) {
  bool queuedMutation = false;
  markerPickerRects_.fill(std::nullopt);
  markerPrefabRects_.fill(std::nullopt);
  const bool hasMarker =
      inspectorSnapshot_.markerStart != "none" || inspectorSnapshot_.markerEnd != "none";
  ImGui::SetNextItemOpen(hasMarker, ImGuiCond_Once);
  const bool markersOpen = ImGui::TreeNodeEx("Markers", ImGuiTreeNodeFlags_SpanAvailWidth);
  const ImVec2 disclosureMin = ImGui::GetItemRectMin();
  const ImVec2 disclosureMax = ImGui::GetItemRectMax();
  markerDisclosureRect_ =
      Box2d(Vector2d(disclosureMin.x, disclosureMin.y), Vector2d(disclosureMax.x, disclosureMax.y));
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
  const std::optional<StrokeMarkerPrefab> currentPrefab =
      context.app != nullptr
          ? StrokeMarkerPrefabForReference(context.app->document().document(), current)
          : std::nullopt;
  std::string display = MarkerDisplayLabel(current);
  if (currentPrefab.has_value()) {
    for (const StrokeMarkerPrefabOption& option : kStrokeMarkerPrefabOptions) {
      if (option.prefab == *currentPrefab) {
        display = option.label;
      }
    }
  }
  ImGui::SetNextItemWidth(144.0f);
  ImGui::PushID(property);
  bool changed = false;
  const bool popupOpen = ImGui::BeginCombo("##marker", display.c_str());
  const ImVec2 pickerMin = ImGui::GetItemRectMin();
  const ImVec2 pickerMax = ImGui::GetItemRectMax();
  markerPickerRects_[std::string_view(property) == "marker-start" ? 0u : 1u] =
      Box2d(Vector2d(pickerMin.x, pickerMin.y), Vector2d(pickerMax.x, pickerMax.y));
  if (popupOpen) {
    if (ImGui::Selectable("None", current == "none") && context.canMutate) {
      changed = applyStrokeStyle(*context.app, property, "none", "Change stroke marker");
    }
    changed = renderMarkerPrefabChoices(context, property, currentPrefab) || changed;
    changed = renderDocumentMarkerChoices(context, property, current) || changed;
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}

bool SidebarPresenter::renderMarkerPrefabChoices(const StrokeRenderContext& context,
                                                 const char* property,
                                                 std::optional<StrokeMarkerPrefab> currentPrefab) {
  ImGui::SeparatorText("Presets");
  bool changed = false;
  for (std::size_t index = 0; index < kStrokeMarkerPrefabOptions.size(); ++index) {
    const StrokeMarkerPrefabOption& option = kStrokeMarkerPrefabOptions[index];
    const bool chosen = ImGui::Selectable(option.label.data(), currentPrefab == option.prefab);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    markerPrefabRects_[index] = Box2d(Vector2d(rowMin.x, rowMin.y), Vector2d(rowMax.x, rowMax.y));
    if (chosen && context.canMutate) {
      changed = ApplyStrokeMarkerPrefab(*context.app, property, option.prefab) || changed;
    }
  }
  return changed;
}

bool SidebarPresenter::renderDocumentMarkerChoices(const StrokeRenderContext& context,
                                                   const char* property,
                                                   const std::string& current) {
  ImGui::SeparatorText("Document markers");
  bool changed = false;
  for (const std::string& id : inspectorSnapshot_.markerIds) {
    const std::string reference = "url(#" + id + ")";
    if (ImGui::Selectable(id.c_str(), current == reference) && context.canMutate) {
      changed =
          applyStrokeStyle(*context.app, property, reference, "Change stroke marker") || changed;
    }
  }
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

bool SidebarPresenter::applyStrokeDashOffset(EditorApp& liveApp, const Lengthd& offset) {
  beginStrokeScalarEdit(liveApp, StrokeScalarField::DashOffset);
  const bool changed =
      liveApp.setStylePropertyOnSelection("stroke-dashoffset", offset.toRcString());
  strokeScalarEdit_->changed |= changed;
  return changed;
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

SidebarPresenter::StrokeEditOwner SidebarPresenter::strokeEditOwner(const EditorApp* app) const {
  if (app != nullptr && app->hasDocument()) {
    return {.document = app->document().document().handle(),
            .documentGeneration = app->document().documentGeneration(),
            .itemId = ImGui::GetItemID()};
  }
  return {.document = inspectorSnapshot_.document,
          .documentGeneration = inspectorSnapshot_.documentGeneration,
          .itemId = ImGui::GetItemID()};
}

void SidebarPresenter::discardForeignStrokeEdits(const EditorApp& app) {
  const auto discard = [&app](auto& edit) {
    if (!edit.has_value()) {
      return;
    }
    const bool sameDocument = app.hasDocument() &&
                              edit->owner.document == app.document().document().handle() &&
                              edit->owner.documentGeneration == app.document().documentGeneration();
    if (!sameDocument) {
      if (ImGui::GetCurrentContext() != nullptr && edit->owner.itemId != 0 &&
          ImGui::GetActiveID() == edit->owner.itemId) {
        ImGui::ClearActiveID();
      }
      edit.reset();
    }
  };
  discard(strokeLengthEditStart_);
  discard(strokeScalarEdit_);
  discardUnstartedStrokeEditForSelection(app);
}

void SidebarPresenter::discardUnstartedStrokeEditForSelection(const EditorApp& app) {
  if (!strokeLengthEditStart_.has_value() || !strokeLengthEditStart_->styles.empty() ||
      strokeLengthEditStart_->selection == app.selectedElements()) {
    return;
  }
  if (ImGui::GetCurrentContext() != nullptr &&
      ImGui::GetActiveID() == strokeLengthEditStart_->owner.itemId) {
    ImGui::ClearActiveID();
  }
  strokeLengthEditStart_.reset();
}

void SidebarPresenter::beginStrokeScalarEdit(EditorApp& liveApp, StrokeScalarField field) {
  discardForeignStrokeEdits(liveApp);
  (void)prepareStrokeScalarEdit(&liveApp, field, nullptr);
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
  discardForeignStrokeEdits(liveApp);
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

  bool queuedMutation = flushPendingTransformValue(liveApp);
  transformFieldRects_.fill(std::nullopt);
  matrixFieldRects_.fill(std::nullopt);

  // Active fields retain input in their edit buffer until document access is available.
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

  const double xValue = bounds.has_value() ? bounds->topLeft.x : 0.0;
  const double yValue = bounds.has_value() ? bounds->topLeft.y : 0.0;
  const double widthValue = bounds.has_value() ? bounds->width() : 0.0;
  const double heightValue = bounds.has_value() ? bounds->height() : 0.0;
  const double rotationValue =
      decomposable ? snapshotDecomposed->rotationRadians * MathConstants<double>::kRadToDeg : 0.0;

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
                                 TransformField field, const char* id, double value, bool canEdit,
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
        const bool canEditCell = visuallyEditable && (liveApp != nullptr || editingThisCell);
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,
                            visuallyEditable ? 1.0f : ImGui::GetStyle().DisabledAlpha);
        if (!canEditCell) {
          ImGui::BeginDisabled();
        }
        const TransformTextTarget cellTarget{this, TransformField::Matrix, i};
        const InspectorNumericEdit cell = DragInspectorNumber(
            kMatrixLabels[i], &value, 0.01f, "%.6g", {&ParseTransformFieldText, &cellTarget},
            TransformFieldHint(TransformField::Matrix, i), theme);
        if (!canEditCell) {
          ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();
        const ImVec2 cellMin = ImGui::GetItemRectMin();
        const ImVec2 cellMax = ImGui::GetItemRectMax();
        matrixFieldRects_[i] =
            Box2d(Vector2d(cellMin.x, cellMin.y), Vector2d(cellMax.x, cellMax.y));

        if (liveApp != nullptr && liveEditable && ImGui::IsItemActivated()) {
          beginTransformEdit(*liveApp, TransformField::Matrix, i, "Edit transform");
        }
        if (transformEdit_.has_value() && transformEdit_->field == TransformField::Matrix &&
            transformEdit_->matrixIndex == i && !transformEdit_->pendingCommit) {
          queuedMutation = queueTransformFieldValue(liveApp, cell, value) || queuedMutation;
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
  const float disabledAlpha = canEdit ? 1.0f : ImGui::GetStyle().DisabledAlpha;
  canEdit = canEdit && (liveApp != nullptr || editingThisField);
  double value = editingThisField ? transformEdit_->fieldValue : static_cast<double>(displayValue);
  const TransformTextTarget textTarget{this, field, 0};

  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, disabledAlpha);
  if (!canEdit) {
    ImGui::BeginDisabled();
  }
  const InspectorNumericEdit edit =
      DragInspectorNumber(label, &value, dragSpeed, format, {&ParseTransformFieldText, &textTarget},
                          TransformFieldHint(field, 0), EditorTheme::Active());
  if (!canEdit) {
    ImGui::EndDisabled();
  }
  ImGui::PopStyleVar();

  bool queuedMutation = false;
  if (liveApp != nullptr && canEdit && ImGui::IsItemActivated()) {
    beginTransformEdit(*liveApp, field, /*matrixIndex=*/0, undoLabel);
  }
  if (transformEdit_.has_value() && transformEdit_->field == field &&
      !transformEdit_->pendingCommit) {
    queuedMutation = queueTransformFieldValue(liveApp, edit, value);
  }
  return queuedMutation;
}

std::optional<double> SidebarPresenter::ParseTransformFieldText(const void* context,
                                                                std::string_view text) {
  const auto* target = static_cast<const TransformTextTarget*>(context);
  return target->presenter->parseTransformFieldText(target->field, target->matrixIndex, text);
}

std::optional<double> SidebarPresenter::parseTransformFieldText(TransformField field,
                                                                int matrixIndex,
                                                                std::string_view text) const {
  if (field == TransformField::Rotation) {
    return ParseInspectorDegrees(text);
  }
  if (field == TransformField::Matrix && matrixIndex < 4) {
    return ParseInspectorNumber(text);
  }
  const bool horizontal = field == TransformField::PositionX || field == TransformField::Width ||
                          (field == TransformField::Matrix && matrixIndex == 4);
  return transformLengthToUserUnits(text, horizontal ? Lengthd::Extent::X : Lengthd::Extent::Y);
}

std::optional<double> SidebarPresenter::transformLengthToUserUnits(std::string_view text,
                                                                   Lengthd::Extent extent) const {
  const std::optional<Lengthd> length = ParseInspectorLength(text, LengthUnit::None);
  if (!length.has_value()) {
    return std::nullopt;
  }
  const Box2d viewBox = transformEdit_.has_value() ? transformEdit_->unitViewBox : Box2d();
  const FontMetrics fontMetrics =
      transformEdit_.has_value() ? transformEdit_->unitFontMetrics : FontMetrics();
  if (IsDocumentSizeRelative(length->unit) && !(viewBox.width() > 0.0 && viewBox.height() > 0.0)) {
    return std::nullopt;
  }
  const double userUnits = length->toPixels(viewBox, fontMetrics, extent);
  return std::isfinite(userUnits) ? std::optional<double>(userUnits) : std::nullopt;
}

bool SidebarPresenter::queueTransformFieldValue(EditorApp* liveApp,
                                                const InspectorNumericEdit& edit, double value) {
  if (edit.typing) {
    transformEdit_->lastTextRejected = edit.rejected;
  }
  double restored = value;
  restoreTransformFieldOnCancel(edit, &restored);
  return updateTransformFieldValue(liveApp, restored, edit.changed || restored != value,
                                   ImGui::IsItemDeactivated());
}

void SidebarPresenter::restoreTransformFieldOnCancel(const InspectorNumericEdit& edit,
                                                     double* value) {
  // Tab or a click on an earlier field deactivates on a frame without the typed text, so the
  // rejection seen on the last typing frame decides.
  if (!transformEdit_.has_value() || !ImGui::IsItemDeactivated() ||
      !(edit.rejected || transformEdit_->lastTextRejected ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
    return;
  }
  TransformEditState& state = *transformEdit_;
  state.restoringStart = true;
  *value = state.field == TransformField::Matrix
               ? state.startTransform.data[static_cast<std::size_t>(state.matrixIndex)]
               : InitialTransformFieldValue(state);
}

void SidebarPresenter::discardForeignTransformEdit(const EditorApp& app) {
  if (!transformEdit_.has_value()) {
    return;
  }
  const bool sameDocument =
      app.hasDocument() &&
      transformEdit_->documentGeneration == app.document().documentGeneration() &&
      transformEdit_->document == app.document().document().handle();
  if (!sameDocument) {
    if (ImGui::GetCurrentContext() != nullptr && transformEdit_->itemId != 0 &&
        ImGui::GetActiveID() == transformEdit_->itemId) {
      ImGui::ClearActiveID();
    }
    transformEdit_.reset();
  }
}

bool SidebarPresenter::flushPendingTransformValue(EditorApp* liveApp) {
  if (liveApp == nullptr) {
    return false;
  }
  discardForeignTransformEdit(*liveApp);
  if (!transformEdit_.has_value() || !transformEdit_->pendingValue) {
    return false;
  }
  transformEdit_->pendingValue = false;
  return applyTransformEdit(*liveApp, transformEdit_->fieldValue);
}

bool SidebarPresenter::updateTransformFieldValue(EditorApp* liveApp, double value, bool changed,
                                                 bool deactivated) {
  if (changed && std::isfinite(value)) {
    transformEdit_->fieldValue = value;
    transformEdit_->pendingValue = true;
    if (transformEdit_->field == TransformField::Matrix) {
      transformEdit_->matrixValues[static_cast<std::size_t>(transformEdit_->matrixIndex)] = value;
    }
  }
  transformEdit_->pendingCommit |= deactivated;
  const bool queued = flushPendingTransformValue(liveApp);
  if (liveApp != nullptr && transformEdit_.has_value() && transformEdit_->pendingCommit) {
    commitTransformEdit(*liveApp);
  }
  return queued;
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
      .element = element,
      .document = liveApp.document().document().handle(),
      .documentGeneration = liveApp.document().documentGeneration(),
      .itemId = ImGui::GetCurrentContext() != nullptr ? ImGui::GetActiveID() : 0,
      .field = field,
      .matrixIndex = matrixIndex,
      .undoLabel = undoLabel};
  // `transform()` and the Inspector bounds may materialize lazy layout state and
  // therefore acquire document write access. Do not wrap these DOM getters in
  // a read scope: ConcurrentDom uses a non-recursive lock, so upgrading that
  // scope would deadlock on field activation.
  state.startTransform = element.cast<svg::SVGGraphicsElement>().transform();
  state.startBounds = InspectorBounds(element);
  if (HasPaintedInspectorBounds(element) &&
      (field == TransformField::Width || field == TransformField::Height)) {
    state.boundsAfterAxisScale = CaptureSelectionBoundsAfterAxisScale(element);
  }
  state.documentFromParent = DocumentFromParentTransform(element);
  if (const double determinant = state.documentFromParent.determinant();
      std::isfinite(determinant) && determinant != 0.0) {
    state.parentFromDocument = state.documentFromParent.inverse();
  }
  // Verbatim attribute bytes so undo restores the user's original source
  // text instead of the canonical serializer output.
  state.sourceTransformAttributeValue = element.getAttribute("transform");
  state.currentTransform = state.startTransform;
  state.startDecomposed = DecomposeTransform(state.startTransform);
  state.unitViewBox = PercentReference(liveApp.document().document());
  state.unitFontMetrics = TypedUnitFontMetrics(liveApp.document().document().svgElement(), element);
  // Captured now, while the source is still in sync with the DOM, so undo
  // and the source writeback resolve the element after document changes.
  state.writebackTarget = captureAttributeWritebackTarget(element);
  for (int i = 0; i < 6; ++i) {
    state.matrixValues[static_cast<std::size_t>(i)] = state.startTransform.data[i];
  }

  state.fieldValue = InitialTransformFieldValue(state);

  transformEdit_ = std::move(state);
}

double SidebarPresenter::InitialTransformFieldValue(const TransformEditState& state) {
  const Box2d bounds = state.startBounds.value_or(Box2d{});
  switch (state.field) {
    case TransformField::PositionX: return bounds.topLeft.x;
    case TransformField::PositionY: return bounds.topLeft.y;
    case TransformField::Width: return bounds.width();
    case TransformField::Height: return bounds.height();
    case TransformField::Rotation:
      return state.startDecomposed.has_value()
                 ? state.startDecomposed->rotationRadians * MathConstants<double>::kRadToDeg
                 : 0.0;
    case TransformField::Matrix:
      return state.matrixValues[static_cast<std::size_t>(state.matrixIndex)];
  }

  UTILS_UNREACHABLE();
}

std::optional<Transform2d> SidebarPresenter::DocumentFieldChange(const TransformEditState& state,
                                                                 double value) {
  switch (state.field) {
    case TransformField::PositionX: return PositionChange(state.startBounds, value, true);
    case TransformField::PositionY: return PositionChange(state.startBounds, value, false);
    case TransformField::Width:
    case TransformField::Height:
      if (state.boundsAfterAxisScale && state.startBounds.has_value()) {
        return PaintedSizeChange(*state.startBounds, value, state.field == TransformField::Width,
                                 state.boundsAfterAxisScale, &state.boundsEvaluationCount);
      }
      return SizeChange(state.startBounds, value, state.field == TransformField::Width);
    case TransformField::Rotation:
    case TransformField::Matrix: return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Transform2d> SidebarPresenter::RotatedParentFromEntity(
    const TransformEditState& state, double value) {
  if (!state.startDecomposed.has_value() || !state.parentFromDocument.has_value()) {
    return std::nullopt;
  }
  // The field shows the rotation of the element's own (parent-relative)
  // transform, so rotate in the parent frame: the displayed angle then becomes
  // exactly the typed value, even under a reflected or non-uniformly scaled
  // ancestor. The pivot is the document-space bounds center mapped into the
  // parent frame, or the element origin when there are no bounds.
  const double deltaRadians =
      value * MathConstants<double>::kDegToRad - state.startDecomposed->rotationRadians;
  const Vector2d pivotInParent =
      state.startBounds.has_value()
          ? state.parentFromDocument->transformPosition(
                (state.startBounds->topLeft + state.startBounds->bottomRight) * 0.5)
          : state.startTransform.translation();
  return state.startTransform * Transform2d::Translate(-pivotInParent) *
         Transform2d::Rotate(deltaRadians) * Transform2d::Translate(pivotInParent);
}

Transform2d SidebarPresenter::composeFieldTransform(const TransformEditState& state,
                                                    double value) const {
  if (state.field == TransformField::Matrix) {
    Transform2d parentFromEntity(Transform2d::uninitialized);
    for (int i = 0; i < 6; ++i) {
      parentFromEntity.data[i] = state.matrixValues[static_cast<std::size_t>(i)];
    }
    return parentFromEntity;
  }
  if (state.field == TransformField::Rotation) {
    const std::optional<Transform2d> parentFromEntity = RotatedParentFromEntity(state, value);
    return parentFromEntity.has_value() && IsFiniteTransform(*parentFromEntity)
               ? *parentFromEntity
               : state.currentTransform;
  }

  // The Position and Size fields are document-space quantities, the
  // same space as the bounds they show. Conjugate the document-space change
  // through the parent's transform before applying it after the element's own
  // transform (Donner transforms apply left to right), matching SelectTool:
  // parentFromEntity' = parentFromEntity * documentFromParent * change *
  // parentFromDocument.
  const std::optional<Transform2d> documentFromStartDocument = DocumentFieldChange(state, value);
  if (!documentFromStartDocument.has_value() || !state.parentFromDocument.has_value()) {
    return state.currentTransform;
  }
  // A pure translation maps through the parent's linear part directly, so a
  // Position edit writes an exact translate instead of a conjugated matrix
  // carrying rounding noise in its linear terms.
  const Transform2d parentFromEntity =
      documentFromStartDocument->isTranslation()
          ? state.startTransform * Transform2d::Translate(state.parentFromDocument->transformVector(
                                       documentFromStartDocument->translation()))
          : state.startTransform * state.documentFromParent * *documentFromStartDocument *
                *state.parentFromDocument;
  return IsFiniteTransform(parentFromEntity) ? parentFromEntity : state.currentTransform;
}

bool SidebarPresenter::applyTransformEdit(EditorApp& liveApp, double value) {
  discardForeignTransformEdit(liveApp);
  if (!transformEdit_.has_value() || !std::isfinite(value)) {
    return false;
  }
  // Recomposing the start value can differ from the start transform by rounding.
  const Transform2d newTransform = transformEdit_->restoringStart
                                       ? transformEdit_->startTransform
                                       : composeFieldTransform(*transformEdit_, value);
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
  discardForeignTransformEdit(liveApp);
  if (!transformEdit_.has_value()) {
    return;
  }
  (void)flushPendingTransformValue(&liveApp);
  // Leaving the field by any route while its text is rejected keeps the value from before typing.
  if (transformEdit_->lastTextRejected && !transformEdit_->restoringStart) {
    transformEdit_->restoringStart = true;
    (void)applyTransformEdit(liveApp, transformEdit_->fieldValue);
  }
  TransformEditState state = std::move(*transformEdit_);
  transformEdit_.reset();
  if (!state.changed) {
    return;
  }

  // A cancelled or rejected entry returns to the starting transform: put the author's original
  // attribute back and leave history untouched.
  if (std::equal(std::begin(state.currentTransform.data), std::end(state.currentTransform.data),
                 std::begin(state.startTransform.data))) {
    if (state.writebackTarget.has_value()) {
      liveApp.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
          .target = *state.writebackTarget,
          .transform = state.startTransform,
          .sourceTransformAttributeValue = state.sourceTransformAttributeValue,
          .restoreSourceTransformAttributeValue = true,
      });
    }
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
