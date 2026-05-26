#define IMGUI_DEFINE_MATH_OPERATORS
#include "donner/editor/EditorShell.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "GLFW/glfw3.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/editor/DocumentSave.h"
#include "donner/editor/DragCoalesce.h"
#include "donner/editor/FocusView.h"
#include "donner/editor/KeyboardShortcutPolicy.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/SourceSelection.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/StyleSourceAnnotations.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/XmlAutocomplete.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReproRecorder.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/properties/PaintServer.h"
#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

namespace donner::editor {

namespace {

constexpr float kMinSourcePaneWidth = 240.0f;
constexpr float kMaxSourcePaneWidth = 900.0f;
constexpr float kSourcePaneSplitterThickness = 6.0f;
constexpr float kSourcePaneRevealHandleWidth = 10.0f;
constexpr float kSourcePaneCollapseThreshold = kMinSourcePaneWidth;
constexpr float kTreeViewHeightFraction = 0.33f;
constexpr float kKeyboardZoomStep = 1.5f;
constexpr float kRightPaneSplitterThickness = 6.0f;
constexpr float kLayerPanelSplitterThickness = 6.0f;
constexpr float kLayerPanelDragHandleHeight = 26.0f;
constexpr float kMinRightPaneWidth = 220.0f;
constexpr float kMaxRightPaneWidth = 900.0f;
constexpr float kMinInspectorPaneHeight = 96.0f;
constexpr float kMinLayerPanelHeight = 140.0f;
constexpr double kTrackpadPanPixelsPerScrollUnit = 10.0;
constexpr double kWheelZoomStep = 1.1;
constexpr int kMaxSaveSyncFlushPasses = 4;
constexpr float kSelectionSizeChipPaddingX = 6.0f;
constexpr float kSelectionSizeChipPaddingY = 3.0f;
constexpr float kSelectionSizeChipRadius = 5.0f;
constexpr float kSelectionSizeChipGapFromAabb = 4.0f;
constexpr float kSelectionSizeChipMinFontSize = 13.0f;
constexpr float kSelectionSizeChipFontStepDown = 2.0f;
constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr float kReferenceChipPaddingX = 8.0f;
constexpr float kReferenceChipPaddingY = 5.0f;
constexpr float kReferenceChipRadius = 6.0f;
constexpr float kReferenceChipGapFromAabb = 30.0f;
constexpr float kReferenceChipMinFontSize = 15.0f;
constexpr float kToolPaletteButtonSize = 30.0f;
constexpr float kToolPaletteGap = 4.0f;
constexpr float kToolPalettePadding = 5.0f;
constexpr float kToolPalettePaintWidgetWidth = 118.0f;
constexpr float kToolPaletteTopInset = 8.0f;
constexpr std::string_view kRenderPaneContextMenuName = "Render Context Menu";
constexpr ImWchar kEditorGlyphRanges[] = {
    0x0020, 0x00ff,  // Basic Latin + Latin Supplement.
    0x2217, 0x2217,  // Asterisk operator.
    0x2731, 0x2731,  // Heavy asterisk.
    0,
};
constexpr ImWchar kEditorSymbolGlyphRanges[] = {
    0x2217, 0x2217,  // Asterisk operator.
    0x2731, 0x2731,  // Heavy asterisk.
    0,
};

ImGuiMouseCursor CursorForTransformHandleIntent(const SelectionTransformHandleIntent& intent) {
  if (intent.kind == SelectionTransformHandleKind::Rotate) {
    return ImGuiMouseCursor_ResizeAll;
  }

  if (intent.kind != SelectionTransformHandleKind::Resize) {
    return ImGuiMouseCursor_Arrow;
  }

  switch (intent.corner) {
    case SelectionTransformCorner::TopLeft:
    case SelectionTransformCorner::BottomRight: return ImGuiMouseCursor_ResizeNWSE;
    case SelectionTransformCorner::TopRight:
    case SelectionTransformCorner::BottomLeft: return ImGuiMouseCursor_ResizeNESW;
  }
  return ImGuiMouseCursor_ResizeAll;
}

void SetImGuiOsCursorManagementEnabled(bool enabled) {
  ImGuiIO& io = ImGui::GetIO();
  if (enabled) {
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
  } else {
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }
}

bool ContainsScreenPoint(const Box2d& rect, const ImVec2& point) {
  return point.x >= rect.topLeft.x && point.x <= rect.bottomRight.x && point.y >= rect.topLeft.y &&
         point.y <= rect.bottomRight.y;
}

ImVec2 ToImVec2(const Vector2d& point) {
  return ImVec2(static_cast<float>(point.x), static_cast<float>(point.y));
}

Vector2d CubicPoint(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3,
                    double t) {
  const double oneMinusT = 1.0 - t;
  return p0 * (oneMinusT * oneMinusT * oneMinusT) + p1 * (3.0 * oneMinusT * oneMinusT * t) +
         p2 * (3.0 * oneMinusT * t * t) + p3 * (t * t * t);
}

ImVec2 PenToolIconPoint(const ImVec2& origin, float scale, float x, float y) {
  return ImVec2(origin.x + x * scale, origin.y + y * scale);
}

void StrokePenToolButtonIcon(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                             ImU32 color, float strokeWidth) {
  const float buttonSize = std::min(max.x - min.x, max.y - min.y);
  const float iconSize = buttonSize - 9.0f;
  const float scale = iconSize / 24.0f;
  const ImVec2 origin(min.x + (max.x - min.x - iconSize) * 0.5f,
                      min.y + (max.y - min.y - iconSize) * 0.5f);
  const auto point = [&](float x, float y) { return PenToolIconPoint(origin, scale, x, y); };

  drawList->PathClear();
  drawList->PathLineTo(point(14.4153f, 18.6964f));
  drawList->PathLineTo(point(7.88293f, 19.2352f));
  drawList->PathLineTo(point(3.80865f, 3.84719f));
  drawList->PathLineTo(point(19.1966f, 7.92147f));
  drawList->PathLineTo(point(18.3043f, 14.8073f));
  drawList->PathStroke(color, ImDrawFlags_None, strokeWidth);

  constexpr float kCosMinus45 = 0.70710678f;
  constexpr float kSinMinus45 = -0.70710678f;
  constexpr float kRectX = 13.7081f;
  constexpr float kRectY = 19.4036f;
  const auto rectPoint = [&](float dx, float dy) {
    return point(kRectX + dx * kCosMinus45 - dy * kSinMinus45,
                 kRectY + dx * kSinMinus45 + dy * kCosMinus45);
  };
  drawList->PathClear();
  drawList->PathLineTo(rectPoint(0.0f, 0.0f));
  drawList->PathLineTo(rectPoint(7.22447f, 0.0f));
  drawList->PathLineTo(rectPoint(7.22447f, 3.0f));
  drawList->PathLineTo(rectPoint(0.0f, 3.0f));
  drawList->PathStroke(color, ImDrawFlags_Closed, strokeWidth);

  drawList->AddLine(point(5.92996f, 5.96852f), point(10.8797f, 10.9183f), color, strokeWidth);
  drawList->AddCircle(point(12.2939f, 12.3325f), 2.0f * scale, color, 0, strokeWidth);
}

void DrawPenToolButtonIcon(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
  StrokePenToolButtonIcon(drawList, min, max, IM_COL32(255, 255, 255, 255), 3.0f);
  StrokePenToolButtonIcon(drawList, min, max, IM_COL32(0, 0, 0, 255), 1.75f);
}

void DrawSelectToolButtonIcon(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
  const float buttonSize = std::min(max.x - min.x, max.y - min.y);
  const float iconSize = buttonSize - 8.0f;
  const float scale = iconSize / 24.0f;
  const ImVec2 origin(min.x + (max.x - min.x - iconSize) * 0.5f,
                      min.y + (max.y - min.y - iconSize) * 0.5f);
  const auto point = [&](float x, float y) {
    return ImVec2(origin.x + x * scale, origin.y + y * scale);
  };
  const std::array<ImVec2, 7> points = {
      point(5.5f, 3.5f),   point(5.5f, 21.0f),  point(10.2f, 16.4f), point(12.9f, 22.0f),
      point(15.8f, 20.6f), point(13.0f, 15.1f), point(19.5f, 15.1f),
  };

  drawList->AddTriangleFilled(points[0], points[1], points[2], IM_COL32(0, 0, 0, 255));
  drawList->AddTriangleFilled(points[0], points[2], points[6], IM_COL32(0, 0, 0, 255));
  drawList->AddTriangleFilled(points[2], points[3], points[5], IM_COL32(0, 0, 0, 255));
  drawList->AddTriangleFilled(points[3], points[4], points[5], IM_COL32(0, 0, 0, 255));

  drawList->AddPolyline(points.data(), static_cast<int>(points.size()),
                        IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 3.2f);
  drawList->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(0, 0, 0, 255),
                        ImDrawFlags_Closed, 1.7f);
}

float ColorChannelToFloat(std::uint8_t value) {
  return static_cast<float>(value) / 255.0f;
}

ImVec4 ColorToImVec4(const css::RGBA& color) {
  return ImVec4(ColorChannelToFloat(color.r), ColorChannelToFloat(color.g),
                ColorChannelToFloat(color.b), ColorChannelToFloat(color.a));
}

css::RGBA ColorFromPicker(const float color[4]) {
  const auto channel = [](float value) {
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
  };
  return css::RGBA(channel(color[0]), channel(color[1]), channel(color[2]), channel(color[3]));
}

std::string ColorToSvgAttribute(const css::RGBA& color) {
  return color.toHexString();
}

std::optional<css::RGBA> ParseColorAttribute(std::string_view value) {
  auto parsed = css::parser::ColorParser::ParseString(value);
  if (parsed.hasError() || !parsed.hasResult()) {
    return std::nullopt;
  }

  const css::Color color = parsed.result();
  if (color.isCurrentColor()) {
    return css::RGBA::RGB(0, 0, 0);
  }

  return color.asRGBA();
}

css::RGBA CurrentColorForElement(const svg::SVGElement& element) {
  const css::Color color = element.getComputedStyle().color.getRequired();
  return color.isCurrentColor() ? css::RGBA::RGB(0, 0, 0) : color.asRGBA();
}

struct ToolbarPaintReferenceState {
  std::string href;
  bool external = false;
  std::optional<SourceByteRange> sourceRange;
};

struct ToolbarPaintSlotState {
  css::RGBA color = css::RGBA::RGB(0, 0, 0);
  bool isNone = true;
  bool isCustom = false;
  std::optional<ToolbarPaintReferenceState> reference;
  std::string customLabel;
};

struct ToolbarPaintState {
  ToolbarPaintSlotState fill;
  ToolbarPaintSlotState stroke;
};

css::RGBA PaintServerFallbackColor() {
  return css::RGBA::RGB(74, 89, 112);
}

ToolbarPaintSlotState ToolbarPaintSlotStateForActiveAttribute(std::string_view value) {
  ToolbarPaintSlotState state;
  if (value == "none") {
    return state;
  }

  state.isNone = false;
  const std::optional<css::RGBA> parsedColor = ParseColorAttribute(value);
  state.color = parsedColor.value_or(PaintServerFallbackColor());
  state.isCustom = !parsedColor.has_value();
  if (state.isCustom) {
    state.customLabel = std::string(value);
  }
  return state;
}

std::optional<SourceByteRange> ResolveReferenceSourceRange(svg::SVGDocument& document,
                                                           std::string_view source,
                                                           const svg::Reference& reference) {
  if (reference.isExternal()) {
    return std::nullopt;
  }

  const std::optional<svg::ResolvedReference> resolved = reference.resolve(document.registry());
  if (!resolved.has_value() || !resolved->valid()) {
    return std::nullopt;
  }

  return EntitySourceByteRange(resolved->handle, source);
}

ToolbarPaintReferenceState ToolbarPaintReferenceStateFor(svg::SVGDocument* document,
                                                         std::optional<std::string_view> source,
                                                         const svg::Reference& reference) {
  ToolbarPaintReferenceState state;
  state.href = std::string(std::string_view(reference.href));
  state.external = reference.isExternal();

  if (document != nullptr && source.has_value() && !state.external) {
    state.sourceRange = ResolveReferenceSourceRange(*document, *source, reference);
  }

  return state;
}

ToolbarPaintSlotState ToolbarPaintSlotStateForPaintServer(const svg::PaintServer& paint,
                                                          const css::RGBA& currentColor,
                                                          svg::SVGDocument* document,
                                                          std::optional<std::string_view> source) {
  ToolbarPaintSlotState state;
  state.color = PaintServerFallbackColor();

  if (paint.is<svg::PaintServer::None>()) {
    state.isNone = true;
    return state;
  }

  if (paint.is<svg::PaintServer::Solid>()) {
    state.isNone = false;
    state.color = paint.get<svg::PaintServer::Solid>().color.resolve(currentColor, 1.0f);
    return state;
  }

  if (paint.is<svg::PaintServer::ElementReference>()) {
    const auto& ref = paint.get<svg::PaintServer::ElementReference>();
    state.isNone = false;
    state.isCustom = true;
    state.reference = ToolbarPaintReferenceStateFor(document, source, ref.reference);
    if (ref.fallback.has_value()) {
      state.color = ref.fallback->resolve(currentColor, 1.0f);
    }
    return state;
  }

  state.isNone = false;
  state.isCustom = true;
  state.customLabel = paint.is<svg::PaintServer::ContextFill>() ? "context-fill" : "context-stroke";
  return state;
}

ToolbarPaintSlotState ToolbarPaintSlotStateForElement(const svg::SVGElement& element,
                                                      std::string_view attrName,
                                                      svg::SVGDocument* document,
                                                      std::optional<std::string_view> source) {
  const auto& style = element.getComputedStyle();
  const svg::PaintServer paint =
      attrName == "fill" ? style.fill.getRequired() : style.stroke.getRequired();
  ToolbarPaintSlotState state =
      ToolbarPaintSlotStateForPaintServer(paint, CurrentColorForElement(element), document, source);

  if (state.isCustom || !state.isNone) {
    return state;
  }

  if (std::optional<RcString> attribute = element.getAttribute(attrName);
      attribute.has_value() && std::string_view(*attribute) != "none") {
    state = ToolbarPaintSlotStateForActiveAttribute(std::string_view(*attribute));
  }
  return state;
}

ToolbarPaintState ToolbarPaintStateForActivePaint(const ActivePaintStyle& paintStyle) {
  ToolbarPaintState state;
  state.fill = ToolbarPaintSlotStateForActiveAttribute(paintStyle.fill);
  state.stroke = ToolbarPaintSlotStateForActiveAttribute(paintStyle.stroke);
  return state;
}

void OverridePaintStateFromElement(ToolbarPaintState* state, const svg::SVGElement& element,
                                   svg::SVGDocument* document,
                                   std::optional<std::string_view> source) {
  state->fill = ToolbarPaintSlotStateForElement(element, "fill", document, source);
  state->stroke = ToolbarPaintSlotStateForElement(element, "stroke", document, source);
}

ToolbarPaintState ToolbarPaintStateForApp(EditorApp& app, std::optional<std::string_view> source) {
  ToolbarPaintState state = ToolbarPaintStateForActivePaint(app.activePaintStyle());
  if (app.hasSelection()) {
    OverridePaintStateFromElement(&state, app.selectedElements().front(),
                                  app.hasDocument() ? &app.document().document() : nullptr, source);
  }
  return state;
}

void DrawPaintSwatch(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                     const ToolbarPaintSlotState& state) {
  drawList->AddRectFilled(min, max, ImGui::GetColorU32(ColorToImVec4(state.color)), 2.0f);
  if (state.isCustom) {
    drawList->PushClipRect(min, max, true);
    for (float x = min.x - (max.y - min.y); x < max.x; x += 5.0f) {
      drawList->AddLine(ImVec2(x, max.y), ImVec2(x + (max.y - min.y), min.y),
                        IM_COL32(255, 255, 255, 95), 1.0f);
    }
    drawList->PopClipRect();
  }
  drawList->AddRect(min, max, IM_COL32(255, 255, 255, 230), 2.0f, 0, 1.0f);
  drawList->AddRect(min, max, state.isCustom ? IM_COL32(91, 189, 255, 255) : IM_COL32(0, 0, 0, 210),
                    2.0f, 0, 2.0f);
  if (state.isNone) {
    drawList->AddLine(ImVec2(min.x + 2.0f, max.y - 2.0f), ImVec2(max.x - 2.0f, min.y + 2.0f),
                      IM_COL32(230, 40, 40, 255), 2.2f);
  }
}

std::string PaintChipLabel(std::string_view prefix, const ToolbarPaintSlotState& state) {
  std::string value = state.reference.has_value() ? state.reference->href : state.customLabel;
  if (value.empty()) {
    value = "custom";
  }
  constexpr std::size_t kMaxValueLength = 12;
  if (value.size() > kMaxValueLength) {
    value = value.substr(0, kMaxValueLength - 3) + "...";
  }
  return std::string(prefix) + " " + value;
}

std::string SelectionSizeChipLabel(const Box2d& screenBounds) {
  const int width = std::max(0, static_cast<int>(std::lround(std::abs(screenBounds.width()))));
  const int height = std::max(0, static_cast<int>(std::lround(std::abs(screenBounds.height()))));
  return std::to_string(width) + " x " + std::to_string(height);
}

std::string SelectionPositionChipLabel(const Box2d& documentBounds) {
  const int x = static_cast<int>(std::lround(documentBounds.topLeft.x));
  const int y = static_cast<int>(std::lround(documentBounds.topLeft.y));
  return "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
}

std::string SelectionAngleChipLabel(const Transform2d& documentFromStartDocument) {
  double degrees =
      std::atan2(documentFromStartDocument.data[1], documentFromStartDocument.data[0]) *
      kRadiansToDegrees;
  while (degrees <= -180.0) {
    degrees += 360.0;
  }
  while (degrees > 180.0) {
    degrees -= 360.0;
  }
  return std::to_string(static_cast<int>(std::lround(degrees))) + " deg";
}

float SelectionSizeChipFontSize() {
  return std::max(kSelectionSizeChipMinFontSize,
                  ImGui::GetFontSize() - kSelectionSizeChipFontStepDown);
}

ImFont* SelectionSizeChipFont(ImFont* uiFontBold) {
  return uiFontBold != nullptr ? uiFontBold : ImGui::GetFont();
}

ImVec2 SelectionSizeChipTextSize(std::string_view label, ImFont* font) {
  return font->CalcTextSizeA(SelectionSizeChipFontSize(), FLT_MAX, -1.0f, label.data(),
                             label.data() + label.size());
}

Box2d TransformDocumentBox(const Box2d& box, const Transform2d& documentFromBoundsDocument) {
  const std::array<Vector2d, 4> corners{
      box.topLeft,
      Vector2d(box.bottomRight.x, box.topLeft.y),
      box.bottomRight,
      Vector2d(box.topLeft.x, box.bottomRight.y),
  };

  Box2d transformed = Box2d::CreateEmpty(documentFromBoundsDocument.transformPosition(corners[0]));
  for (const Vector2d& corner : corners) {
    transformed.addPoint(documentFromBoundsDocument.transformPosition(corner));
  }
  return transformed;
}

float ClampSourcePaneWidthForWindow(float requestedWidth, float windowWidth) {
  const float sourcePaneUpperBound = std::max(
      0.0f, std::min(kMaxSourcePaneWidth, windowWidth - kMinRightPaneWidth - kMinRightPaneWidth));
  const float sourcePaneLowerBound = std::min(kMinSourcePaneWidth, sourcePaneUpperBound);
  return std::clamp(requestedWidth, sourcePaneLowerBound, sourcePaneUpperBound);
}

std::string ReferenceHighlightChipLabel(const ReferenceHighlightSummary& summary) {
  std::vector<std::string> parts;
  if (!summary.referencedElements.empty()) {
    parts.push_back("-> " + std::to_string(summary.referencedElements.size()));
  }
  if (!summary.referencingElements.empty()) {
    parts.push_back("<- " + std::to_string(summary.referencingElements.size()));
  }

  std::string label;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      label += "  ";
    }
    label += parts[i];
  }
  return label;
}

float ReferenceHighlightChipFontSize() {
  return std::max(kReferenceChipMinFontSize, ImGui::GetFontSize());
}

ImVec2 ReferenceHighlightChipTextSize(std::string_view label) {
  ImFont* font = ImGui::GetFont();
  return font->CalcTextSizeA(ReferenceHighlightChipFontSize(), FLT_MAX, -1.0f, label.data(),
                             label.data() + label.size());
}

void AddUniqueElements(std::vector<svg::SVGElement>* target,
                       std::span<const svg::SVGElement> elements) {
  for (const svg::SVGElement& element : elements) {
    const Entity entity = element.entityHandle().entity();
    const auto it = std::ranges::find_if(*target, [entity](const svg::SVGElement& existing) {
      return existing.entityHandle().entity() == entity;
    });
    if (it == target->end()) {
      target->push_back(element);
    }
  }
}

bool ContainsElement(std::span<const svg::SVGElement> elements, const svg::SVGElement& element) {
  return std::ranges::find(elements, element) != elements.end();
}

std::string ElementContextMenuLabel(const svg::SVGElement& element) {
  const std::string_view tagName = element.tagName().name;
  std::string label = "<";
  label.append(tagName.data(), tagName.size());
  label.push_back('>');

  const RcString id = element.id();
  const std::string_view idSv = id;
  if (!idSv.empty()) {
    label.push_back(' ');
    label.push_back('#');
    label.append(idSv.data(), idSv.size());
  }

  return label;
}

std::optional<std::string> LoadFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream out;
  out << file.rdbuf();
  return std::move(out).str();
}

std::string InitialDocumentSyncSource(const EditorShellOptions& options) {
  if (options.initialSource.has_value()) {
    return *options.initialSource;
  }
  return LoadFile(options.svgPath).value_or("");
}

std::string CanonicalizeForTextEditor(std::string_view source) {
  std::string result(source);
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }
  return result;
}

Box2d ResolveDocumentViewBox(svg::SVGDocument& document) {
  if (auto viewBox = document.svgElement().viewBox(); viewBox.has_value()) {
    return *viewBox;
  }
  const Vector2i intrinsic = document.canvasSize();
  if (intrinsic.x > 0 && intrinsic.y > 0) {
    return Box2d::FromXYWH(0.0, 0.0, static_cast<double>(intrinsic.x),
                           static_cast<double>(intrinsic.y));
  }
  return Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
}

}  // namespace

EditorShell::EditorShell(gui::EditorWindow& window, EditorShellOptions options)
    : window_(window),
      options_(std::move(options)),
      app_(),
      selectTool_(),
      penTool_(),
      textEditor_(),
      textures_(window.geodeDevice()),
      renderCoordinator_(window.geodeDevice()),
      rotateCursorSet_(),
      documentSyncController_(InitialDocumentSyncSource(options_)),
      interactionController_(),
      inputBridge_(window_, kWheelZoomStep),
      layerInspectorPanel_(window.geodeDevice()),
      dialogPresenter_(options_.editorNoticeText) {
  std::optional<std::string> initialSource = options_.initialSource;
  if (!initialSource.has_value() && !options_.svgPath.empty()) {
    initialSource = LoadFile(options_.svgPath);
  }
  if (!initialSource.has_value()) {
    return;
  }

  textEditor_.setLanguageDefinition(TextEditor::LanguageDefinition::SVG());
  textEditor_.setText(*initialSource);
  textEditor_.resetTextChanged();
  textEditor_.setActiveAutocomplete(true);
  textEditor_.setAutocompleteProvider([](const TextEditor::AutocompleteRequest& request)
                                          -> std::optional<TextEditor::AutocompleteResponse> {
    XmlAutocompleteContext context =
        DetectXmlAutocompleteContext(request.source, request.cursorOffset);
    if (context.kind == XmlAutocompleteContextKind::Unknown) {
      return std::nullopt;
    }

    TextEditor::AutocompleteResponse response;
    response.replaceStartOffset = context.replaceStartOffset;
    response.replaceEndOffset = context.replaceEndOffset;
    for (const XmlAutocompleteSuggestion& suggestion : BuildXmlAutocompleteSuggestions(context)) {
      response.suggestions.push_back(TextEditor::AutocompleteSuggestion{
          .displayText = RcString(suggestion.displayText),
          .insertText = RcString(suggestion.insertText),
      });
    }
    return response;
  });

  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig fontCfg;
  fontCfg.FontDataOwnedByAtlas = false;
  const double displayScale = window_.displayScale();
  std::ignore = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(embedded::kRobotoRegularTtf.data()),
      static_cast<int>(embedded::kRobotoRegularTtf.size()), static_cast<float>(15.0 * displayScale),
      &fontCfg, kEditorGlyphRanges);
  uiFontBold_ = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(embedded::kRobotoBoldTtf.data()),
      static_cast<int>(embedded::kRobotoBoldTtf.size()), static_cast<float>(15.0 * displayScale),
      &fontCfg, kEditorGlyphRanges);
  codeFont_ = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(embedded::kFiraCodeRegularTtf.data()),
      static_cast<int>(embedded::kFiraCodeRegularTtf.size()),
      static_cast<float>(14.0 * displayScale), &fontCfg, kEditorGlyphRanges);
  ImFontConfig codeSymbolFontCfg = fontCfg;
  codeSymbolFontCfg.MergeMode = true;
  std::ignore = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(embedded::kRobotoRegularTtf.data()),
      static_cast<int>(embedded::kRobotoRegularTtf.size()), static_cast<float>(14.0 * displayScale),
      &codeSymbolFontCfg, kEditorSymbolGlyphRanges);

  if (!app_.loadFromString(*initialSource)) {
    // Keep the shell alive so the user can still edit/fix the file from the source pane.
  }
  if (options_.initialPath.has_value()) {
    app_.setCurrentFilePath(*options_.initialPath);
  } else if (!options_.svgPath.empty()) {
    app_.setCurrentFilePath(options_.svgPath);
  }
  // Route the clean baseline through `textEditor_.getText()` so it matches
  // what `syncDirtyFromSource` will later compare against. `TextBuffer`
  // canonicalizes line endings (e.g. drops a trailing `\n`), so comparing
  // against the raw file bytes would leave the dirty flag latched on.
  app_.setCleanSourceText(textEditor_.getText());
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  textures_.initialize();
  if (!rotateCursorSet_.initialize(window_.rawHandle(), window_.geodeDevice())) {
    std::fprintf(stderr, "[editor] custom rotate cursor unavailable; using fallback cursor\n");
  }

  // On-demand render loop: the main thread sleeps in `window.waitEvents()`
  // between user inputs, so the worker thread has to nudge it when a
  // render finishes — otherwise the fresh bitmap sits in `result_`
  // forever. Safe to capture `this` because `AsyncRenderer`'s lifetime
  // is strictly nested inside `RenderCoordinator`'s, which is a member
  // of `*this`.
  renderCoordinator_.asyncRenderer().setWakeCallback([this]() { window_.wakeEventLoop(); });

  if (options_.reproOutputPath.has_value()) {
    repro::ReproRecorderOptions recorderOptions;
    recorderOptions.outputPath = *options_.reproOutputPath;
    recorderOptions.svgPath = options_.svgPath;
    recorderOptions.svgSource = *initialSource;
    const auto winSize = window_.windowSize();
    recorderOptions.windowWidth = winSize.x;
    recorderOptions.windowHeight = winSize.y;
    recorderOptions.displayScale = window_.displayScale();
    reproRecorder_ = std::make_unique<repro::ReproRecorder>(std::move(recorderOptions));
    std::fprintf(stderr, "[repro] recording UI inputs to %s\n", options_.reproOutputPath->c_str());
  }

  valid_ = true;
}

std::optional<float> EditorShell::nextIdleWakeSeconds() const {
  std::optional<float> result;
  const auto includeWake = [&result](std::optional<float> wakeSeconds) {
    if (!wakeSeconds.has_value()) {
      return;
    }

    const float clampedWakeSeconds = std::max(0.0f, *wakeSeconds);
    result = result.has_value() ? std::min(*result, clampedWakeSeconds) : clampedWakeSeconds;
  };

  includeWake(documentSyncController_.nextTextSyncWakeSeconds());
  includeWake(textEditor_.nextFlashWakeSeconds());
  includeWake(textEditor_.nextRopeAnimationWakeSeconds());
  return result;
}

EditorShell::~EditorShell() {
  if (reproRecorder_) {
    if (!reproRecorder_->flush()) {
      std::fprintf(stderr, "[repro] flush failed — recording lost\n");
    }
  }
}

void EditorShell::overrideViewportForReplay(const ViewportState& viewport) {
  pendingViewportReplayOverride_ = viewport;
}

std::optional<std::string> EditorShell::selectedElementLabelForReadback() const {
  const std::optional<svg::SVGElement>& selected = app_.selectedElement();
  if (!selected.has_value()) {
    return std::nullopt;
  }

  const std::string_view tagName = selected->tagName().name;
  std::string label = "<";
  label.append(tagName.data(), tagName.size());
  label.push_back('>');

  const RcString id = selected->id();
  const std::string_view idSv = id;
  if (!idSv.empty()) {
    label.push_back(' ');
    label.push_back('#');
    label.append(idSv.data(), idSv.size());
  }
  return label;
}

LayerInspectorStatusReadback EditorShell::layerInspectorStatusForReadback() const {
  const auto& viewport = interactionController_.viewport();
  const Vector2i viewportDesiredCanvas = viewport.desiredCanvasSize();
  const bool workerBusy = renderCoordinator_.asyncRenderer().isBusy();
  const Vector2i documentCanvas = (!workerBusy && app_.hasDocument())
                                      ? app_.document().document().canvasSize()
                                      : renderCoordinator_.asyncRenderer().lastDocumentCanvasSize();
  const Vector2i compositorCanvas = renderCoordinator_.asyncRenderer().compositorState().canvasSize;
  const CanvasFreshness freshness =
      ClassifyCanvasFreshness(viewportDesiredCanvas, documentCanvas, compositorCanvas);
  LayerInspectorStatusReadback readback{
      .canvasFreshness = freshness,
      .statusSuffix = std::string(CanvasFreshnessStatusSuffix(freshness)),
      .viewportDesiredCanvas = viewportDesiredCanvas,
      .documentCanvas = documentCanvas,
      .compositorCanvas = compositorCanvas,
      .metadataOnlyMissCount = textures_.metadataOnlyMissCount(),
      .duplicateLiveTextureCount = textures_.duplicateLiveTextureCount(),
      .overlayDimsPx = Vector2i(textures_.overlayWidth(), textures_.overlayHeight()),
      .overlayTextureHandle = static_cast<std::uint64_t>(textures_.overlayTexture()),
  };
  const std::optional<SelectTool::ActiveDragPreview> liveActiveDragPreview =
      selectTool_.activeDragPreview();
  const std::optional<SelectTool::ActiveDragPreview> activeDragPreview =
      renderCoordinator_.compositedPresentation().activePreviewForPresentation(
          liveActiveDragPreview);
  const std::optional<SelectTool::ActiveDragPreview> displayedDragPreview =
      renderCoordinator_.compositedPresentation().presentationPreview(activeDragPreview);
  readback.tiles.reserve(textures_.tiles().size());
  for (const GlTextureCache::TileView& tile : textures_.tiles()) {
    Vector2d presentedDragTranslationDoc = tile.dragTranslationDoc;
    if (tile.isDragTarget && activeDragPreview.has_value() && displayedDragPreview.has_value() &&
        activeDragPreview->entity == displayedDragPreview->entity) {
      presentedDragTranslationDoc +=
          activeDragPreview->translation - displayedDragPreview->translation;
    }
    readback.tiles.push_back(LayerInspectorStatusReadback::Tile{
        .id = tile.id,
        .kind = tile.kind,
        .generation = tile.generation,
        .bitmapDimsPx = tile.bitmapDimsPx,
        .rasterCanvasSize = tile.rasterCanvasSize,
        .canvasOffsetDoc = tile.canvasOffsetDoc,
        .bitmapDimsDoc = tile.bitmapDimsDoc,
        .dragTranslationDoc = tile.dragTranslationDoc,
        .presentedDragTranslationDoc = presentedDragTranslationDoc,
        .textureHandle = static_cast<std::uint64_t>(tile.texture),
        .metadataOnly = tile.metadataOnly,
        .isDragTarget = tile.isDragTarget,
    });
  }
  return readback;
}

bool EditorShell::tryOpenPath(std::string_view path, std::string* error) {
  auto contents = LoadFile(std::string(path));
  if (!contents.has_value()) {
    *error = "Could not open file.";
    return false;
  }
  if (!app_.loadFromString(*contents)) {
    *error = "Failed to parse SVG.";
    return false;
  }

  textEditor_.setText(*contents);
  textEditor_.resetTextChanged();
  const std::string canonicalSource = textEditor_.getText();
  app_.setCurrentFilePath(std::string(path));
  resetPresentationForLoadedDocument(canonicalSource);
  return true;
}

void EditorShell::resetPresentationForLoadedDocument(std::string_view canonicalSource) {
  documentSyncController_.resetForLoadedDocument(std::string(canonicalSource));
  app_.setCleanSourceText(canonicalSource);
  lastHighlightedSelection_.clear();
  lastTreeSelection_.reset();
  textEditor_.clearFocusPartition();
  lastPostedScreenPoint_.reset();
  preserveSourceEditFocusCursor_ = false;
  sourceSelectionOriginatedInText_ = false;
  sourceFocusOriginatedInStyle_ = false;
  referenceHighlightActive_ = false;
  referenceHighlightChipHovered_ = false;
  referenceHighlightSummary_ = ReferenceHighlightSummary{};
  lastReferenceHighlightSelection_.clear();
  renderContextMenuDocumentPoint_.reset();
  renderContextMenuHitElement_.reset();
  renderContextMenuOpenRequested_ = false;
  treeSelectionOriginatedInTree_ = false;
  treeviewPendingScroll_ = false;
  renderCoordinator_.resetForLoadedDocument();
  textures_.resetComposited();
  textures_.clearOverlay();
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  dialogPresenter_.clearOpenFileError();
  dialogPresenter_.clearSaveFileError();
}

bool EditorShell::synchronizeSourceBeforeSave(std::string* error) {
  documentSyncController_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/1.0f);

  if (renderCoordinator_.asyncRenderer().isBusy()) {
    *error = "Cannot save while the renderer is applying pending edits.";
    return false;
  }

  for (int pass = 0; pass < kMaxSaveSyncFlushPasses; ++pass) {
    if (!app_.flushFrame()) {
      break;
    }
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    documentSyncController_.applyPendingWritebacks(app_, selectTool_, textEditor_);
    documentSyncController_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/1.0f);
  }

  if (!app_.document().queue().empty()) {
    *error = "Cannot save while document edits are still pending.";
    return false;
  }

  return true;
}

bool EditorShell::trySavePath(std::string_view path, std::string* error) {
  if (path.empty()) {
    *error = "Choose a file path.";
    return false;
  }
  if (!app_.hasDocument()) {
    *error = "No SVG document is loaded.";
    return false;
  }
  if (!synchronizeSourceBeforeSave(error)) {
    return false;
  }
  if (!app_.document().document().hasSourceStore()) {
    *error = "The current document has no XML source store.";
    return false;
  }

  const DocumentSaveResult result = SaveSourceToPath(std::filesystem::path(std::string(path)),
                                                     app_.document().document().source());
  if (!result.ok()) {
    *error = result.message;
    return false;
  }

  app_.setCurrentFilePath(std::string(path));
  app_.setCleanSourceText(textEditor_.getText());
  documentSyncController_.resetForLoadedDocument(textEditor_.getText());
  dialogPresenter_.clearSaveFileError();
  updateWindowTitle();
  return true;
}

void EditorShell::requestSaveAs(std::string error) {
  dialogPresenter_.requestSaveFile(app_.currentFilePath(), std::move(error));
}

void EditorShell::requestSave() {
  if (!app_.currentFilePath().has_value()) {
    requestSaveAs();
    return;
  }

  std::string error;
  if (!trySavePath(*app_.currentFilePath(), &error)) {
    requestSaveAs(std::move(error));
  }
}

void EditorShell::requestRevert() {
  if (!app_.hasDocument() || !app_.isDirty() || app_.cleanSourceText().empty()) {
    return;
  }

  const std::string source(app_.cleanSourceText());
  if (!app_.revertToCleanSource()) {
    return;
  }

  textEditor_.setText(source);
  textEditor_.resetTextChanged();
  resetPresentationForLoadedDocument(textEditor_.getText());
  updateWindowTitle();
  window_.wakeEventLoop();
}

void EditorShell::updateWindowTitle() {
  std::string title;
  if (app_.isDirty()) {
    title += "● ";
  }
  if (app_.currentFilePath().has_value()) {
    title += std::filesystem::path(*app_.currentFilePath()).filename().string();
  } else {
    title += "untitled";
  }
  title += " - Donner SVG Editor";
  if (title != lastWindowTitle_) {
    window_.setTitle(title);
    lastWindowTitle_ = std::move(title);
  }
}

void EditorShell::handleGlobalShortcuts() {
  const bool anyPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
  const bool cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
  const bool shift = ImGui::GetIO().KeyShift;
  const bool pressedZ = ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false);
  const bool pressedEnter = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
  const bool sourcePaneFocused = sourcePaneVisible_ && textEditor_.isFocused();

  if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_O, /*repeat=*/false)) {
    dialogPresenter_.requestOpenFile(app_.currentFilePath());
  }

  if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_Q, /*repeat=*/false)) {
    glfwSetWindowShouldClose(window_.rawHandle(), GLFW_TRUE);
  }

  if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_S, /*repeat=*/false)) {
    if (shift) {
      requestSaveAs();
    } else {
      requestSave();
    }
  }

  if (!sourcePaneFocused) {
    if (pressedZ && cmd && !shift) {
      if (app_.canUndo()) {
        app_.undo();
      }
    } else if (pressedZ && cmd && shift && app_.canRedo()) {
      app_.redo();
    }
  }

  if (!anyPopupOpen && cmd &&
      (ImGui::IsKeyPressed(ImGuiKey_Equal, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, /*repeat=*/false))) {
    interactionController_.applyZoom(kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (!anyPopupOpen && cmd &&
      (ImGui::IsKeyPressed(ImGuiKey_Minus, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, /*repeat=*/false))) {
    interactionController_.applyZoom(1.0 / kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_0, /*repeat=*/false)) {
    interactionController_.resetToActualSize();
  }

  if (CanToggleSourceFocusModeFromShortcut(pressedEnter, cmd, anyPopupOpen)) {
    toggleSourceFocusMode();
  }

  if (!sourcePaneFocused && !anyPopupOpen && !cmd &&
      ImGui::IsKeyPressed(ImGuiKey_V, /*repeat=*/false)) {
    activeTool_ = ActiveTool::Select;
    penTool_.cancel();
  }

  if (!sourcePaneFocused && !anyPopupOpen && !cmd &&
      ImGui::IsKeyPressed(ImGuiKey_P, /*repeat=*/false)) {
    activeTool_ = ActiveTool::Pen;
  }

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      penTool_.isDrafting()) {
    penTool_.cancel();
    return;
  }

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      app_.hasSelection()) {
    app_.setSelection(std::nullopt);
  }

  const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
  if (CanDeleteSelectedElementsFromShortcut(deleteKey, app_.hasSelection(), anyPopupOpen,
                                            sourcePaneFocused)) {
    std::ignore = app_.deleteSelectionWithUndo(textEditor_.getText());
  }
}

void EditorShell::renderSourcePane(float paneOriginY, float paneHeight, float paneWidth,
                                   ImFont* codeFont) {
  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  ImGui::SetNextWindowPos(ImVec2(0.0f, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(paneWidth, paneHeight), ImGuiCond_Always);
  ImGui::Begin("Source", nullptr, kPaneFlags);
  ImGui::PushFont(codeFont);
  textEditor_.setSourceFocusModeContextMenu(sourceFocusMode_);
  updateSourceStyleDecorations();
  textEditor_.render("##source");
  applySourceStyleDecorationChipClick();
  updateSourceHoverPreview();
  if (textEditor_.takeSourceFocusModeContextMenuToggleRequest()) {
    toggleSourceFocusMode();
  }
  syncSelectionFromSourceCursorIfNeeded();
  ImGui::PopFont();
  const bool sourceEditShouldPreserveCursor =
      textEditor_.isTextChanged() && sourceFocusMode_ && textEditor_.isCursorInsideFocusRange();
  if (textEditor_.isTextChanged()) {
    preserveSourceEditFocusCursor_ = sourceEditShouldPreserveCursor;
  }
  const std::vector<svg::SVGElement> selectionBeforeTextSync = app_.selectedElements();
  documentSyncController_.handleTextEdits(app_, textEditor_, ImGui::GetIO().DeltaTime);
  if (sourceEditShouldPreserveCursor && app_.selectedElements() != selectionBeforeTextSync) {
    sourceSelectionOriginatedInText_ = true;
  }
  updateSourceHoverPreview();
  ImGui::End();
}

Box2d EditorShell::toolPaletteScreenRect(const ImVec2& paneOrigin,
                                         const ImVec2& contentRegion) const {
  constexpr float kButtonCount = 3.0f;
  const float width = kToolPalettePadding * 2.0f + kToolPaletteButtonSize * kButtonCount +
                      kToolPalettePaintWidgetWidth + kToolPaletteGap * kButtonCount;
  const float height = kToolPalettePadding * 2.0f + kToolPaletteButtonSize;
  const float x = paneOrigin.x + std::max(0.0f, (contentRegion.x - width) * 0.5f);
  const float y = paneOrigin.y + kToolPaletteTopInset;
  return Box2d::FromXYWH(x, y, width, height);
}

void EditorShell::renderFillStrokeToolbarWidget() {
  const bool rendererBusy = renderCoordinator_.asyncRenderer().isBusy();
  const bool canEditPaint = app_.hasDocument() && !rendererBusy;
  std::string editorSource;
  std::string documentSource;
  std::optional<std::string_view> sourceForRanges;
  if (canEditPaint) {
    editorSource = textEditor_.getText();
    documentSource = CanonicalizeForTextEditor(app_.document().document().source());
    if (editorSource == documentSource) {
      sourceForRanges = std::string_view(editorSource);
    }
  }
  const ToolbarPaintState paintState =
      rendererBusy ? ToolbarPaintState{} : ToolbarPaintStateForApp(app_, sourceForRanges);
  ImGui::BeginDisabled(!canEditPaint);
  ImGui::InvisibleButton("##fill_stroke_widget",
                         ImVec2(kToolPalettePaintWidgetWidth, kToolPaletteButtonSize));
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const ImVec2 mouse = ImGui::GetMousePos();
  const ImVec2 strokeMin(min.x + 15.0f, min.y + 3.0f);
  const ImVec2 strokeMax(strokeMin.x + 19.0f, strokeMin.y + 19.0f);
  const ImVec2 fillMin(min.x + 5.0f, min.y + 10.0f);
  const ImVec2 fillMax(fillMin.x + 19.0f, fillMin.y + 19.0f);
  const ImVec2 chipMin(min.x + 42.0f, min.y + 1.0f);
  const ImVec2 chipMax(max.x - 3.0f, min.y + 14.0f);
  const ImVec2 fillChipMin(chipMin.x, min.y + 16.0f);
  const ImVec2 fillChipMax(chipMax.x, min.y + 29.0f);
  const ImVec2 strokeChipMin(chipMin.x, chipMin.y);
  const ImVec2 strokeChipMax(chipMax.x, chipMax.y);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  DrawPaintSwatch(drawList, strokeMin, strokeMax, paintState.stroke);
  DrawPaintSwatch(drawList, fillMin, fillMax, paintState.fill);

  const auto contains = [](const ImVec2& rectMin, const ImVec2& rectMax, const ImVec2& point) {
    return point.x >= rectMin.x && point.x <= rectMax.x && point.y >= rectMin.y &&
           point.y <= rectMax.y;
  };
  const auto fitChipLabel = [](std::string label, float maxWidth) {
    if (ImGui::CalcTextSize(label.c_str()).x <= maxWidth) {
      return label;
    }

    std::string base = std::move(label);
    while (base.size() > 1u) {
      base.pop_back();
      const std::string candidate = base + "...";
      if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) {
        return candidate;
      }
    }
    return std::string("...");
  };
  const auto renderChip = [&](std::string_view prefix, const ToolbarPaintSlotState& slot,
                              const ImVec2& rectMin, const ImVec2& rectMax) {
    if (!slot.isCustom) {
      return;
    }

    const bool actionable = slot.reference.has_value() && slot.reference->sourceRange.has_value();
    const ImU32 fillColor = actionable ? IM_COL32(37, 112, 172, 245) : IM_COL32(61, 72, 86, 225);
    const ImU32 borderColor =
        actionable ? IM_COL32(127, 203, 255, 255) : IM_COL32(119, 132, 150, 235);
    drawList->AddRectFilled(rectMin, rectMax, fillColor, 4.0f);
    drawList->AddRect(rectMin, rectMax, borderColor, 4.0f, 0, 1.0f);

    const std::string label =
        fitChipLabel(PaintChipLabel(prefix, slot), rectMax.x - rectMin.x - 8.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    drawList->AddText(
        ImVec2(rectMin.x + 4.0f, rectMin.y + (rectMax.y - rectMin.y - textSize.y) * 0.5f - 0.5f),
        IM_COL32(255, 255, 255, 245), label.c_str());
  };
  renderChip("S", paintState.stroke, strokeChipMin, strokeChipMax);
  renderChip("F", paintState.fill, fillChipMin, fillChipMax);

  if (canEditPaint && ImGui::IsItemClicked()) {
    const bool clickedFillChip =
        paintState.fill.isCustom && contains(fillChipMin, fillChipMax, mouse);
    const bool clickedStrokeChip =
        paintState.stroke.isCustom && contains(strokeChipMin, strokeChipMax, mouse);
    bool handledPaintClick = false;
    if (clickedFillChip || clickedStrokeChip) {
      const ToolbarPaintSlotState& slot = clickedFillChip ? paintState.fill : paintState.stroke;
      if (slot.reference.has_value() && slot.reference->sourceRange.has_value()) {
        revealSourceRange(*slot.reference->sourceRange);
      }
      handledPaintClick = true;
    }

    if (!handledPaintClick) {
      const bool clickedFill = mouse.x >= fillMin.x && mouse.x <= fillMax.x &&
                               mouse.y >= fillMin.y && mouse.y <= fillMax.y;
      const bool clickedStroke = mouse.x >= strokeMin.x && mouse.x <= strokeMax.x &&
                                 mouse.y >= strokeMin.y && mouse.y <= strokeMax.y;
      if (clickedFill || !clickedStroke) {
        ImGui::OpenPopup("##fill_color_picker");
      } else {
        ImGui::OpenPopup("##stroke_color_picker");
      }
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    const bool hoveredFillChip =
        paintState.fill.isCustom && contains(fillChipMin, fillChipMax, mouse);
    const bool hoveredStrokeChip =
        paintState.stroke.isCustom && contains(strokeChipMin, strokeChipMax, mouse);
    if (hoveredFillChip || hoveredStrokeChip) {
      const char* name = hoveredFillChip ? "Fill" : "Stroke";
      const ToolbarPaintSlotState& slot = hoveredFillChip ? paintState.fill : paintState.stroke;
      if (slot.reference.has_value() && slot.reference->sourceRange.has_value()) {
        ImGui::SetTooltip("%s paint server %s. Click to show source.", name,
                          slot.reference->href.c_str());
      } else if (slot.reference.has_value() && slot.reference->external) {
        ImGui::SetTooltip("%s uses external paint server %s.", name, slot.reference->href.c_str());
      } else if (slot.reference.has_value()) {
        ImGui::SetTooltip("%s uses unresolved paint server %s.", name,
                          slot.reference->href.c_str());
      } else {
        ImGui::SetTooltip("%s uses custom paint %s.", name, slot.customLabel.c_str());
      }
    } else {
      ImGui::SetTooltip("%s", canEditPaint ? "Fill / stroke" : "Open an SVG document");
    }
  }
  ImGui::EndDisabled();

  auto renderColorPopup = [&](const char* popupId, const char* pickerId, std::string_view attrName,
                              const ToolbarPaintSlotState& slot) {
    if (!ImGui::BeginPopup(popupId)) {
      return;
    }

    float pickerColor[4] = {
        ColorChannelToFloat(slot.color.r),
        ColorChannelToFloat(slot.color.g),
        ColorChannelToFloat(slot.color.b),
        ColorChannelToFloat(slot.color.a),
    };
    constexpr ImGuiColorEditFlags kFlags = ImGuiColorEditFlags_AlphaBar |
                                           ImGuiColorEditFlags_AlphaPreviewHalf |
                                           ImGuiColorEditFlags_NoSidePreview;
    if (ImGui::ColorPicker4(pickerId, pickerColor, kFlags)) {
      const css::RGBA chosen = ColorFromPicker(pickerColor);
      const std::string svgColor = ColorToSvgAttribute(chosen);
      if (attrName == "fill") {
        app_.setActiveFill(svgColor);
      } else {
        app_.setActiveStroke(svgColor);
      }
      std::ignore = app_.setStylePropertyOnSelection(attrName, svgColor);
      window_.wakeEventLoop();
    }
    ImGui::EndPopup();
  };

  renderColorPopup("##fill_color_picker", "##fill_picker", "fill", paintState.fill);
  renderColorPopup("##stroke_color_picker", "##stroke_picker", "stroke", paintState.stroke);
}

void EditorShell::renderToolPalette(const ImVec2& paneOrigin, const ImVec2& contentRegion) {
  const Box2d rect = toolPaletteScreenRect(paneOrigin, contentRegion);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(
      ImVec2(static_cast<float>(rect.topLeft.x), static_cast<float>(rect.topLeft.y)),
      ImVec2(static_cast<float>(rect.bottomRight.x), static_cast<float>(rect.bottomRight.y)),
      ImGui::GetColorU32(ImVec4(0.11f, 0.12f, 0.14f, 0.92f)), 7.0f);
  drawList->AddRect(
      ImVec2(static_cast<float>(rect.topLeft.x), static_cast<float>(rect.topLeft.y)),
      ImVec2(static_cast<float>(rect.bottomRight.x), static_cast<float>(rect.bottomRight.y)),
      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f)), 7.0f);

  ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(rect.topLeft.x) + kToolPalettePadding,
                                   static_cast<float>(rect.topLeft.y) + kToolPalettePadding));
  ImGui::PushID("tool_palette");
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

  enum class ToolButtonIcon {
    None,
    SelectPointer,
    PenTool,
  };
  const auto renderButton = [&](ActiveTool tool, const char* label, ToolButtonIcon icon,
                                const char* tooltip) {
    const bool selected = activeTool_ == tool;
    if (selected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.43f, 0.90f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.50f, 1.0f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.35f, 0.78f, 1.0f));
    }
    if (ImGui::Button(label, ImVec2(kToolPaletteButtonSize, kToolPaletteButtonSize))) {
      activeTool_ = tool;
      if (tool == ActiveTool::Select) {
        penTool_.cancel();
      }
    }
    if (icon == ToolButtonIcon::SelectPointer) {
      DrawSelectToolButtonIcon(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    } else if (icon == ToolButtonIcon::PenTool) {
      DrawPenToolButtonIcon(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    if (selected) {
      ImGui::PopStyleColor(3);
    }
  };

  renderButton(ActiveTool::Select, "##select_tool", ToolButtonIcon::SelectPointer, "Select");
  ImGui::SameLine(0.0f, kToolPaletteGap);
  renderButton(ActiveTool::Pen, "##pen_tool", ToolButtonIcon::PenTool, "Pen");
  ImGui::SameLine(0.0f, kToolPaletteGap);
  ImGui::BeginDisabled(true);
  (void)ImGui::Button("△", ImVec2(kToolPaletteButtonSize, kToolPaletteButtonSize));
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", "Path edit");
  }
  ImGui::SameLine(0.0f, kToolPaletteGap);
  renderFillStrokeToolbarWidget();

  ImGui::PopStyleVar(2);
  ImGui::PopID();
}

void EditorShell::renderRenderPane(const Vector2d& renderPaneOrigin, const Vector2d& renderPaneSize,
                                   ImGuiWindowFlags paneFlags) {
  ImGui::SetNextWindowPos(
      ImVec2(static_cast<float>(renderPaneOrigin.x), static_cast<float>(renderPaneOrigin.y)),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(static_cast<float>(renderPaneSize.x), static_cast<float>(renderPaneSize.y)),
      ImGuiCond_Always);
  ImGui::Begin("Render", nullptr, paneFlags);

  const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
  const ImVec2 paneOriginImGui = ImGui::GetCursorScreenPos();
  interactionController_.updatePaneLayout(
      Vector2d(paneOriginImGui.x, paneOriginImGui.y), Vector2d(contentRegion.x, contentRegion.y),
      app_.hasDocument() ? std::make_optional(ResolveDocumentViewBox(app_.document().document()))
                         : std::nullopt);
  interactionController_.updateDevicePixelRatio(window_.contentScale().x);

  if (pendingViewportReplayOverride_.has_value()) {
    interactionController_.viewport() = *pendingViewportReplayOverride_;
    pendingViewportReplayOverride_.reset();
    viewportInitialized_ = true;
  }

  if (!viewportInitialized_ && interactionController_.viewport().paneSize.x > 0.0 &&
      interactionController_.viewport().paneSize.y > 0.0 && app_.hasDocument()) {
    interactionController_.resetToActualSize();
    viewportInitialized_ = true;
  }

  refreshReferenceHighlightSummaryIfNeeded();
  const std::string referenceChipLabel = ReferenceHighlightChipLabel(referenceHighlightSummary_);
  const bool hideReferenceChip = selectTool_.activeTransformBoundsPreview().has_value();
  const std::optional<Box2d> referenceChipRect =
      hideReferenceChip ? std::nullopt : referenceHighlightChipScreenRect(referenceChipLabel);
  const Box2d toolPaletteRect = toolPaletteScreenRect(paneOriginImGui, contentRegion);
  const bool toolPaletteHovered = ContainsScreenPoint(toolPaletteRect, ImGui::GetMousePos());
  const bool referenceChipHovered = referenceChipRect.has_value() &&
                                    ContainsScreenPoint(*referenceChipRect, ImGui::GetMousePos());

  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##render_canvas", contentRegion,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool paneHovered = ImGui::IsItemHovered();
  const bool canvasHovered = paneHovered && !referenceChipHovered && !toolPaletteHovered;
  const Box2d paneRect = Box2d::FromXYWH(interactionController_.viewport().paneOrigin.x,
                                         interactionController_.viewport().paneOrigin.y,
                                         interactionController_.viewport().paneSize.x,
                                         interactionController_.viewport().paneSize.y);

  const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
  const bool middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
  const auto setActiveRotateCursor =
      [&](const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
        if (!activeGesturePreview.has_value() ||
            activeGesturePreview->kind != SelectTool::ActiveGestureKind::Rotate) {
          return false;
        }

        if (rotateCursorSet_.setRotateCursor(activeGesturePreview->corner)) {
          SetImGuiOsCursorManagementEnabled(false);
        } else {
          rotateCursorSet_.clearIfActive();
          SetImGuiOsCursorManagementEnabled(true);
          ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        return true;
      };
  const auto activeGesturePreviewBeforeInput = selectTool_.activeGesturePreview();
  const bool rotateCursorLocked = setActiveRotateCursor(activeGesturePreviewBeforeInput);
  interactionController_.updatePanState(canvasHovered, spaceHeld, middleDown,
                                        ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                        ImGui::GetMousePos());
  const bool showPanCursor =
      !rotateCursorLocked &&
      ShouldShowRenderPanePanCursor(canvasHovered, spaceHeld, interactionController_.panning());
  if (showPanCursor) {
    const PanCursorKind panCursorKind =
        interactionController_.panning() ? PanCursorKind::ClosedHand : PanCursorKind::OpenHand;
    if (rotateCursorSet_.setPanCursor(panCursorKind)) {
      SetImGuiOsCursorManagementEnabled(false);
    } else {
      SetImGuiOsCursorManagementEnabled(true);
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
  }

  const bool modalCapturingInput = referenceChipHovered || toolPaletteHovered ||
                                   ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
  interactionController_.consumeScrollEvents(inputBridge_.events(), paneRect, modalCapturingInput,
                                             kWheelZoomStep, kTrackpadPanPixelsPerScrollUnit);

  const auto screenToDocument = [&](const ImVec2& screenPoint) -> Vector2d {
    return interactionController_.viewport().screenToDocument(
        Vector2d(screenPoint.x, screenPoint.y));
  };

  if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right, /*repeat=*/false)) {
    openRenderPaneContextMenu(screenToDocument(ImGui::GetMousePos()));
  }

  const bool toolEligible = canvasHovered && !interactionController_.panning() && !spaceHeld;
  const bool selectToolActive = activeTool_ == ActiveTool::Select;
  const bool penToolActive = activeTool_ == ActiveTool::Pen;
  const auto cachedHandleIntentAt = [&](const Vector2d& documentPoint) {
    const auto& boundsCache = renderCoordinator_.selectionBoundsCache();
    if (boundsCache.lastSelection != app_.selectedElements()) {
      return SelectionTransformHandleIntent{};
    }
    return HitTestSelectionTransformHandles(boundsCache.displayedBoundsDoc, documentPoint,
                                            interactionController_.viewport().pixelsPerDocUnit());
  };
  SelectionTransformHandleIntent hoverTransformIntent;
  if (penToolActive && !rotateCursorLocked && toolEligible) {
    if (rotateCursorSet_.setPenCursor()) {
      SetImGuiOsCursorManagementEnabled(false);
    } else {
      rotateCursorSet_.clearIfActive();
      SetImGuiOsCursorManagementEnabled(true);
      ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
  } else if (selectToolActive && !rotateCursorLocked && toolEligible &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    hoverTransformIntent = cachedHandleIntentAt(screenToDocument(ImGui::GetMousePos()));
    if (hoverTransformIntent.kind != SelectionTransformHandleKind::None) {
      if (hoverTransformIntent.kind == SelectionTransformHandleKind::Rotate &&
          rotateCursorSet_.setRotateCursor(hoverTransformIntent.corner)) {
        SetImGuiOsCursorManagementEnabled(false);
      } else {
        rotateCursorSet_.clearIfActive();
        SetImGuiOsCursorManagementEnabled(true);
        ImGui::SetMouseCursor(CursorForTransformHandleIntent(hoverTransformIntent));
      }
    } else {
      rotateCursorSet_.clearIfActive();
      SetImGuiOsCursorManagementEnabled(true);
    }
  } else if (!rotateCursorLocked && !toolEligible && !showPanCursor) {
    rotateCursorSet_.clearIfActive();
    SetImGuiOsCursorManagementEnabled(true);
  }
  if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    preserveSourceEditFocusCursor_ = false;
    MouseModifiers modifiers;
    modifiers.shift = ImGui::GetIO().KeyShift;
    modifiers.option = ImGui::GetIO().KeyAlt;
    modifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
    interactionController_.bufferPendingClick(screenToDocument(ImGui::GetMousePos()), modifiers);
  }

  // Design doc 0033 §M8 — click→drag handoff doesn't wait for raster.
  //
  // Fast path: if the user clicks inside the bounds of the currently-
  // selected element and outside cached later-painted bounds, we can
  // start the re-drag IMMEDIATELY without gating on `!isBusy()`. The
  // check uses `SelectionBoundsCache` — populated on idle frames — so
  // the call doesn't touch the registry the worker is mid-mutating. The
  // previous M8 attempt failed because it called the live
  // `SnapshotSelectionWorldBounds` during the busy window; the
  // cache-based check fixes the race.
  //
  // Slow path: anything else (selection change, marquee, shift-click,
  // empty cache, multi-select) still waits for `!isBusy()` and goes
  // through the full `onMouseDown` flow. The follow-up registry-
  // reading work (`refreshSelectionBoundsCache`, overlay rasterize,
  // render request) is deferred to the next idle frame for both
  // paths, so the user sees the click acknowledged immediately even
  // when the chrome catches up a frame later.
  if (interactionController_.pendingClick().has_value()) {
    const auto& pendingClick = *interactionController_.pendingClick();
    const auto& boundsCache = renderCoordinator_.selectionBoundsCache();
    const bool cacheMatchesSelection = boundsCache.lastSelection == app_.selectedElements();
    const SelectionTransformHandleIntent pendingHandleIntent =
        cacheMatchesSelection ? cachedHandleIntentAt(pendingClick.documentPoint)
                              : SelectionTransformHandleIntent{};
    const bool tookFastRedrag =
        selectToolActive && cacheMatchesSelection &&
        pendingHandleIntent.kind == SelectionTransformHandleKind::None &&
        selectTool_.tryStartRedragOnSelected(app_, pendingClick.documentPoint,
                                             pendingClick.modifiers, boundsCache.displayedBoundsDoc,
                                             boundsCache.displayedOccludingBoundsDoc);
    if (tookFastRedrag) {
      lastPostedScreenPoint_.reset();
      interactionController_.clearPendingClick();
      pendingClickFollowupAfterIdle_ = true;
    } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
      // Slow path: full `onMouseDown` (hitTest + selection change +
      // possible drag start). Race-safe only when the worker is idle.
      lastPostedScreenPoint_.reset();
      bool queuedMutationForNextFrame = false;
      if (selectToolActive) {
        selectTool_.onMouseDown(app_, pendingClick.documentPoint, pendingClick.modifiers);
      } else if (penToolActive) {
        penTool_.onMouseDown(app_, pendingClick.documentPoint, pendingClick.modifiers);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && penTool_.isDraggingAnchor()) {
          penTool_.onMouseUp(app_, pendingClick.documentPoint);
        }
        queuedMutationForNextFrame = true;
      }
      if (queuedMutationForNextFrame) {
        window_.wakeEventLoop();
      } else {
        renderCoordinator_.refreshSelectionBoundsCache(app_);
        renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                              textures_);
        renderCoordinator_.rasterizeOverlayForCurrentSelection(
            app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
            RenderCoordinator::OverlayUploadMode::MatchDisplayedVersion,
            selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview());
      }
      interactionController_.clearPendingClick();
    } else {
      // Worker is busy with a (likely-stale) prewarm render at the
      // previous canvas size or zoom. Cancel it so the next idle frame
      // can run the slow-path mouseDown immediately, rather than
      // waiting up to seconds for the in-flight prewarm to finish at
      // high zoom. The render in flight is dispensable — it was a
      // selection prewarm, not a drag, and the click is about to
      // supersede the selection state anyway.
      renderCoordinator_.asyncRenderer().cancelInFlight();
    }
  }

  // After-idle follow-up for the M8 fast-path click. This reads the
  // live registry, so it must wait for the worker to land. Keep the
  // follow-up to cache refresh only: posting a render here would run
  // before this same frame's drag move is applied, leaving the overlay
  // one interaction step behind the composited pixels during re-drag.
  if (pendingClickFollowupAfterIdle_ && !renderCoordinator_.asyncRenderer().isBusy()) {
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    pendingClickFollowupAfterIdle_ = false;
  }

  if (selectTool_.isDragging() || selectTool_.isMarqueeing()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
      const ImVec2 currentScreen = ImGui::GetMousePos();
      if (ShouldPostDragMove<ImVec2>(currentScreen, lastPostedScreenPoint_,
                                     renderCoordinator_.asyncRenderer().isBusy())) {
        MouseModifiers modifiers;
        modifiers.shift = ImGui::GetIO().KeyShift;
        modifiers.option = ImGui::GetIO().KeyAlt;
        modifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
        selectTool_.onMouseMove(app_, screenToDocument(currentScreen), /*buttonHeld=*/true,
                                modifiers);
        lastPostedScreenPoint_ = currentScreen;
        if (!renderCoordinator_.asyncRenderer().isBusy() && app_.flushFrame()) {
          renderCoordinator_.refreshSelectionBoundsCache(app_);
          renderCoordinator_.rasterizeOverlayForCurrentSelection(
              app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
              RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
              selectTool_.activeTransformBoundsPreview());
        }
      }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const auto previewBeforeRelease = selectTool_.activeDragPreview();
      const bool previewHadVisualChange =
          previewBeforeRelease.has_value() &&
          (!previewBeforeRelease->documentFromCachedDocument.isIdentity() ||
           previewBeforeRelease->translation != Vector2d::Zero());
      selectTool_.onMouseUp(app_, screenToDocument(ImGui::GetMousePos()));
      lastPostedScreenPoint_.reset();
      if (previewBeforeRelease.has_value()) {
        if (previewHadVisualChange) {
          renderCoordinator_.compositedPresentation().beginSettling(
              previewBeforeRelease, app_.document().currentFrameVersion());
        }
        if (!renderCoordinator_.asyncRenderer().isBusy() &&
            (app_.flushFrame() || previewHadVisualChange)) {
          renderCoordinator_.compositedPresentation().beginSettling(
              previewBeforeRelease, app_.document().currentFrameVersion());
          renderCoordinator_.refreshSelectionBoundsCache(app_);
          renderCoordinator_.rasterizeOverlayForCurrentSelection(
              app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
              RenderCoordinator::OverlayUploadMode::Immediate);
        }
      } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
        renderCoordinator_.refreshSelectionBoundsCache(app_);
        renderCoordinator_.rasterizeOverlayForCurrentSelection(
            app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect());
      }
    }
  }

  if (penToolActive && penTool_.isDraggingAnchor()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
      penTool_.onMouseMove(app_, screenToDocument(ImGui::GetMousePos()), /*buttonHeld=*/true);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      penTool_.onMouseUp(app_, screenToDocument(ImGui::GetMousePos()));
      window_.wakeEventLoop();
    }
  }

  if (!renderCoordinator_.asyncRenderer().isBusy() && app_.hasDocument()) {
    renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                          textures_);
  }

  const auto liveActiveDragPreview = selectTool_.activeDragPreview();
  const auto activeGesturePreview = selectTool_.activeGesturePreview();
  if (!setActiveRotateCursor(activeGesturePreview) && rotateCursorLocked) {
    rotateCursorSet_.clearIfActive();
    SetImGuiOsCursorManagementEnabled(true);
  }
  const auto activeDragPreview =
      renderCoordinator_.compositedPresentation().activePreviewForPresentation(
          liveActiveDragPreview);
  const auto displayedDragPreview =
      renderCoordinator_.compositedPresentation().presentationPreview(activeDragPreview);
  RenderPanePresenterState paneState{
      .viewport = interactionController_.viewport(),
      .frameHistory = interactionController_.frameHistory(),
      .textures = textures_,
      .activeDragPreview = activeDragPreview,
      .displayedDragPreview = displayedDragPreview,
      .overlayDragPreview = renderCoordinator_.presentedOverlayDragPreview(),
      .contentRegion = Vector2d(contentRegion.x, contentRegion.y),
      .suppressedLayerEntity = renderCoordinator_.suppressedCompositedLayerEntity(app_),
      .suppressDragTargetTiles = renderCoordinator_.selectedElementIsDisplayNone(app_),
  };
  renderPanePresenter_.render(paneState);
  renderPenToolPreview();
  renderSelectionSizeChip(hoverTransformIntent, activeGesturePreview);
  renderReferenceHighlightChip();
  renderToolPalette(paneOriginImGui, contentRegion);
  renderRenderPaneContextMenu();

  ImGui::End();
}

void EditorShell::renderSidebars(float rightPaneX, float rightPaneWidth, float paneOriginY,
                                 const RightSidebarLayout& layout, ImGuiWindowFlags paneFlags) {
  const auto& selectionBeforeTree = app_.selectedElement();
  if (selectionBeforeTree != lastTreeSelection_) {
    treeviewPendingScroll_ = selectionBeforeTree.has_value() && !treeSelectionOriginatedInTree_;
  }
  treeSelectionOriginatedInTree_ = false;

  // Refresh the sidebar snapshot only when the async renderer is idle —
  // during render the worker thread may be mutating registry state the
  // snapshot walk would read. The snapshot persists across the busy window
  // so the panes keep showing their last-known content instead of flashing
  // to "(rendering…)" placeholders.
  const bool rendererBusy = renderCoordinator_.asyncRenderer().isBusy();
  if (!rendererBusy) {
    sidebarPresenter_.refreshSnapshot(app_);
  }
  EditorApp* liveAppForClicks = rendererBusy ? nullptr : &app_;

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.treePaneHeight), ImGuiCond_Always);
  ImGui::Begin("Tree View", nullptr, paneFlags);
  TreeViewState treeState{
      .scrollTarget = selectionBeforeTree,
      .pendingScroll = treeviewPendingScroll_,
  };
  sidebarPresenter_.renderTreeView(liveAppForClicks, treeState);
  treeviewPendingScroll_ = treeState.pendingScroll;
  if (treeState.selectionChangedInTree) {
    preserveSourceEditFocusCursor_ = false;
    treeSelectionOriginatedInTree_ = true;
    treeviewPendingScroll_ = false;
  }
  ImGui::End();
  lastTreeSelection_ = app_.selectedElement();

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.inspectorPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.inspectorPaneHeight), ImGuiCond_Always);
  ImGui::Begin("Inspector", nullptr, paneFlags);
  const bool inspectorQueuedMutation =
      sidebarPresenter_.renderInspector(liveAppForClicks, interactionController_.viewport());
  ImGui::End();
  if (inspectorQueuedMutation) {
    window_.wakeEventLoop();
  }

  if (layerPanelDetached_) {
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.layerPanelPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.layerPanelHeight), ImGuiCond_Always);
  ImGui::Begin("Layers##docked_layers", nullptr, paneFlags);
  renderDockedLayerPanelDragHandle();
  if (!layerPanelDetached_) {
    renderLayerPanelContents();
  }
  ImGui::End();
}

void EditorShell::renderLayerPanelContents() {
  const auto compositeTiles = renderCoordinator_.asyncRenderer().compositorCompositeTiles();
  const auto compositorState = renderCoordinator_.asyncRenderer().compositorState();
  const auto workerCompositorEntity = renderCoordinator_.asyncRenderer().workerCompositorEntity();
  const auto& viewport = interactionController_.viewport();
  const Vector2i viewportDesiredCanvas = viewport.desiredCanvasSize();
  // `SVGDocument::canvasSize()` walks the registry (ComputedAbsoluteTransform /
  // SizedElement / ViewBox) — racy against the worker's
  // `prepareDocumentForRendering` which rebuilds those components in place.
  // When the worker is busy we have to read the cached value the worker
  // stamped at the end of its last completed render; reading live trips a
  // SIGSEGV inside `LayoutSystem::calculateCanvasScaledDocumentSize` when
  // the entt sparse-set page is mid-rebuild.
  const bool workerBusy = renderCoordinator_.asyncRenderer().isBusy();
  const Vector2i documentCanvas = (!workerBusy && app_.hasDocument())
                                      ? app_.document().document().canvasSize()
                                      : renderCoordinator_.asyncRenderer().lastDocumentCanvasSize();
  const auto fastPath = renderCoordinator_.asyncRenderer().compositorFastPathCountersForTesting();
  layerInspectorPanel_.render(compositeTiles, compositorState, workerCompositorEntity,
                              viewport.zoom, viewport.devicePixelRatio, viewportDesiredCanvas,
                              documentCanvas, fastPath);
}

void EditorShell::renderSourcePaneSplitter(float windowWidth, float paneOriginY, float paneHeight,
                                           float sourcePaneWidth) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kSplitterFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;

  if (sourcePaneVisible_ && sourcePaneWidth > 0.0f) {
    const float splitterLeft = sourcePaneWidth - kSourcePaneSplitterThickness * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(splitterLeft, paneOriginY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneSplitterThickness, paneHeight), ImGuiCond_Always);
    ImGui::Begin("##source_pane_splitter", nullptr, kSplitterFlags);
    ImGui::InvisibleButton("##source_pane_splitter_handle",
                           ImVec2(kSourcePaneSplitterThickness, paneHeight));
    if (ImGui::IsItemActive()) {
      const float nextWidth = sourcePaneWidth + ImGui::GetIO().MouseDelta.x;
      if (nextWidth < kSourcePaneCollapseThreshold) {
        setSourcePaneVisible(false);
      } else {
        sourcePaneWidth_ = ClampSourcePaneWidthForWindow(nextWidth, windowWidth);
        window_.wakeEventLoop();
      }
    }
  } else {
    ImGui::SetNextWindowPos(ImVec2(0.0f, paneOriginY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneRevealHandleWidth, paneHeight), ImGuiCond_Always);
    ImGui::Begin("##source_pane_reveal_handle", nullptr, kSplitterFlags);
    ImGui::InvisibleButton("##source_pane_reveal_handle",
                           ImVec2(kSourcePaneRevealHandleWidth, paneHeight));
    if (ImGui::IsItemActive()) {
      const float nextWidth = std::max(0.0f, ImGui::GetMousePos().x);
      if (nextWidth >= kSourcePaneCollapseThreshold) {
        sourcePaneWidth_ = ClampSourcePaneWidthForWindow(nextWidth, windowWidth);
        setSourcePaneVisible(true);
      }
    }
  }

  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }

  const bool splitterActive = ImGui::IsItemActive();
  const bool splitterHovered = ImGui::IsItemHovered();
  const ImU32 color = ImGui::GetColorU32(splitterActive    ? ImGuiCol_SeparatorActive
                                         : splitterHovered ? ImGuiCol_SeparatorHovered
                                                           : ImGuiCol_Separator);
  const ImVec2 itemMin = ImGui::GetItemRectMin();
  const ImVec2 itemMax = ImGui::GetItemRectMax();
  if (sourcePaneVisible_) {
    ImGui::GetWindowDrawList()->AddRectFilled(itemMin, itemMax, color);
  } else if (splitterHovered || splitterActive) {
    ImGui::GetWindowDrawList()->AddRectFilled(itemMin, ImVec2(itemMin.x + 2.0f, itemMax.y), color);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void EditorShell::renderRightPaneSplitter(float windowWidth, float paneOriginY, float paneHeight) {
  const float splitterCenterX = windowWidth - rightPaneWidth_;
  const float splitterLeft = splitterCenterX - kRightPaneSplitterThickness * 0.5f;

  ImGui::SetNextWindowPos(ImVec2(splitterLeft, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(kRightPaneSplitterThickness, paneHeight), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kSplitterFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
  ImGui::Begin("##right_pane_splitter", nullptr, kSplitterFlags);
  ImGui::InvisibleButton("##right_pane_splitter_handle",
                         ImVec2(kRightPaneSplitterThickness, paneHeight));
  if (ImGui::IsItemActive()) {
    const float deltaX = ImGui::GetIO().MouseDelta.x;
    // Dragging the splitter LEFT (negative deltaX) widens the right pane.
    rightPaneWidth_ = std::clamp(rightPaneWidth_ - deltaX, kMinRightPaneWidth, kMaxRightPaneWidth);
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void EditorShell::renderLayerPanelSplitter(float rightPaneX, float rightPaneWidth,
                                           const RightSidebarLayout& layout) {
  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.layerPanelSplitterY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, kLayerPanelSplitterThickness), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kSplitterFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
  ImGui::Begin("##layer_panel_splitter", nullptr, kSplitterFlags);
  ImGui::InvisibleButton("##layer_panel_splitter_handle",
                         ImVec2(rightPaneWidth, kLayerPanelSplitterThickness));
  if (ImGui::IsItemActive()) {
    layerPanelHeightFraction_ = ResizeLayerPanelHeightFraction(
        layerPanelHeightFraction_, layout.lowerPaneHeight, layout.minLayerPanelHeight,
        layout.maxLayerPanelHeight, ImGui::GetIO().MouseDelta.y);
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  }

  const bool splitterActive = ImGui::IsItemActive();
  const bool splitterHovered = ImGui::IsItemHovered();
  const ImU32 color = ImGui::GetColorU32(splitterActive    ? ImGuiCol_SeparatorActive
                                         : splitterHovered ? ImGuiCol_SeparatorHovered
                                                           : ImGuiCol_Separator);
  ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            color);
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void EditorShell::renderDockedLayerPanelDragHandle() {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 size(ImGui::GetContentRegionAvail().x, kLayerPanelDragHandleHeight);
  ImGui::InvisibleButton("##layer_panel_detach_handle", size);

  const bool handleActive = ImGui::IsItemActive();
  const bool handleHovered = ImGui::IsItemHovered();
  if (handleHovered || handleActive) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  }

  const ImU32 background = ImGui::GetColorU32(handleActive    ? ImGuiCol_HeaderActive
                                              : handleHovered ? ImGuiCol_HeaderHovered
                                                              : ImGuiCol_Header);
  const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 end(start.x + size.x, start.y + size.y);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(start, end, background);
  drawList->AddText(ImVec2(start.x + style.FramePadding.x, start.y + style.FramePadding.y),
                    textColor, "Layers");

  const char* handleText = "::";
  const ImVec2 handleTextSize = ImGui::CalcTextSize(handleText);
  drawList->AddText(
      ImVec2(end.x - handleTextSize.x - style.FramePadding.x, start.y + style.FramePadding.y),
      textColor, handleText);

  if (handleActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
    layerPanelDetached_ = true;
    layerPanelDetachDragActive_ = true;
    layerPanelFloatingNeedsPlacement_ = true;
    layerPanelFloatingPos_ = ImGui::GetWindowPos();
    layerPanelFloatingSize_ = ImGui::GetWindowSize();
  }
}

void EditorShell::renderFloatingLayerPanel() {
  if (!layerPanelDetached_) {
    return;
  }

  if (layerPanelDetachDragActive_) {
    const ImGuiIO& io = ImGui::GetIO();
    layerPanelFloatingPos_.x += io.MouseDelta.x;
    layerPanelFloatingPos_.y += io.MouseDelta.y;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      layerPanelDetachDragActive_ = false;
    }
  }

  if (layerPanelFloatingNeedsPlacement_ || layerPanelDetachDragActive_) {
    ImGui::SetNextWindowPos(layerPanelFloatingPos_, ImGuiCond_Always);
  }
  if (layerPanelFloatingNeedsPlacement_) {
    ImGui::SetNextWindowSize(layerPanelFloatingSize_, ImGuiCond_Always);
  }

  bool layerPanelOpen = true;
  constexpr ImGuiWindowFlags kFloatingFlags = ImGuiWindowFlags_NoCollapse;
  ImGui::Begin("Layers##floating_layers", &layerPanelOpen, kFloatingFlags);
  layerPanelFloatingNeedsPlacement_ = false;
  layerPanelFloatingPos_ = ImGui::GetWindowPos();
  layerPanelFloatingSize_ = ImGui::GetWindowSize();

  if (!layerPanelOpen || ImGui::Button("Dock")) {
    layerPanelDetached_ = false;
    layerPanelDetachDragActive_ = false;
    layerPanelFloatingNeedsPlacement_ = false;
    ImGui::End();
    return;
  }

  renderLayerPanelContents();
  ImGui::End();
}

bool EditorShell::highlightSelectionSourceIfNeeded() {
  const auto& selectionNow = app_.selectedElements();
  const bool preserveSourceEditCursor =
      preserveSourceEditFocusCursor_ && sourceFocusMode_ && textEditor_.isCursorInsideFocusRange();
  if (selectionNow != lastHighlightedSelection_) {
    referenceHighlightActive_ = false;
    referenceHighlightChipHovered_ = false;
    referenceHighlightSummary_ = ReferenceHighlightSummary{};
    lastReferenceHighlightSelection_.clear();
    if (sourceSelectionOriginatedInText_ || preserveSourceEditCursor) {
      if (!sourceFocusOriginatedInStyle_) {
        updateSourceFocusView(/*scrollToSelection=*/false);
      }
      sourceSelectionOriginatedInText_ = false;
      preserveSourceEditFocusCursor_ = false;
    } else if (!selectionNow.empty()) {
      updateSourceFocusView(/*scrollToSelection=*/true);
    } else {
      textEditor_.clearFocusPartition();
    }
    lastHighlightedSelection_ = selectionNow;
    return true;
  }

  if (preserveSourceEditFocusCursor_ &&
      (!sourceFocusMode_ || !textEditor_.isCursorInsideFocusRange())) {
    preserveSourceEditFocusCursor_ = false;
  }

  return false;
}

std::optional<StyleFocus> EditorShell::styleFocusAtSourceOffset(std::size_t sourceOffset) const {
  std::optional<StyleFocus> styleFocus =
      ComputeStyleFocusAtSourceOffset(app_.document().document(), sourceOffset);
  if (!styleFocus.has_value() && sourceOffset > 0) {
    styleFocus = ComputeStyleFocusAtSourceOffset(app_.document().document(), sourceOffset - 1);
  }

  return styleFocus;
}

std::vector<svg::SVGElement> EditorShell::sourceHoverElements() const {
  if (!app_.hasDocument() || textEditor_.isTextChanged() || app_.document().hasPendingMutations()) {
    return {};
  }

  const std::optional<Coordinates> hoverPosition = textEditor_.hoveredTextPosition();
  if (!hoverPosition.has_value()) {
    return {};
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  const std::string editorSource = textEditor_.getText();
  if (editorSource != documentSource) {
    return {};
  }

  const std::size_t hoverOffset = textEditor_.getByteOffsetAtCoordinates(*hoverPosition);
  if (std::optional<StyleFocus> styleFocus = styleFocusAtSourceOffset(hoverOffset)) {
    return ExcludeDocumentRootSourceHoverElement(
        ExcludeSelectedSourceHoverElements(std::move(styleFocus->impactedElements),
                                           app_.selectedElements()),
        app_.document().document());
  }

  std::optional<svg::SVGElement> element =
      FindElementNearSourceOffset(app_.document().document(), editorSource, hoverOffset);
  if (!element.has_value()) {
    return {};
  }

  return ExcludeDocumentRootSourceHoverElement(
      ExcludeSelectedSourceHoverElements({*element}, app_.selectedElements()),
      app_.document().document());
}

std::vector<SourceByteRange> EditorShell::sourceHoverRangesForElements(
    const std::vector<svg::SVGElement>& elements) const {
  if (elements.empty() || !app_.hasDocument() || textEditor_.isTextChanged()) {
    return {};
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (textEditor_.getText() != documentSource) {
    return {};
  }

  std::vector<SourceByteRange> ranges;
  ranges.reserve(elements.size());
  for (const svg::SVGElement& element : elements) {
    if (std::optional<SourceByteRange> range = ElementSourceByteRange(element, documentSource)) {
      ranges.push_back(*range);
    }
  }
  return ranges;
}

std::vector<svg::SVGElement> EditorShell::referenceHighlightElements() const {
  std::vector<svg::SVGElement> elements;
  if (!referenceHighlightActive_ && !referenceHighlightChipHovered_) {
    return elements;
  }

  elements.reserve(referenceHighlightSummary_.totalCount());
  AddUniqueElements(&elements, referenceHighlightSummary_.referencedElements);
  AddUniqueElements(&elements, referenceHighlightSummary_.referencingElements);
  return elements;
}

std::vector<svg::SVGElement> EditorShell::combinedSourcePreviewElements() const {
  std::vector<svg::SVGElement> elements = sourceHoverElements();
  AddUniqueElements(&elements, referenceHighlightElements());
  return elements;
}

void EditorShell::updateSourceHoverPreview() {
  if (renderCoordinator_.asyncRenderer().isBusy()) {
    const bool overlayChanged = renderCoordinator_.setSourceHoverElements({});
    const bool sourceChanged = textEditor_.clearHoverSourceRanges();
    if (overlayChanged || sourceChanged) {
      window_.wakeEventLoop();
    }
    return;
  }

  const std::vector<svg::SVGElement> hoverElements = combinedSourcePreviewElements();
  const bool overlayChanged = renderCoordinator_.setSourceHoverElements(hoverElements);
  const bool sourceChanged =
      textEditor_.setHoverSourceRanges(sourceHoverRangesForElements(hoverElements));
  if (overlayChanged || sourceChanged) {
    window_.wakeEventLoop();
  }
}

void EditorShell::refreshReferenceHighlightSummaryIfNeeded() {
  const std::vector<svg::SVGElement>& selection = app_.selectedElements();
  if (selection == lastReferenceHighlightSelection_) {
    return;
  }

  referenceHighlightActive_ = false;
  referenceHighlightChipHovered_ = false;
  referenceHighlightSummary_ = ReferenceHighlightSummary{};
  if (!app_.hasDocument() || selection.empty()) {
    lastReferenceHighlightSelection_ = selection;
    return;
  }

  if (renderCoordinator_.asyncRenderer().isBusy()) {
    window_.wakeEventLoop();
    return;
  }

  lastReferenceHighlightSelection_ = selection;
  referenceHighlightSummary_ = ComputeReferenceHighlightSummary(
      app_.document().document(), std::span<const svg::SVGElement>(selection));
}

void EditorShell::applyReferenceHighlightPreview() {
  if (renderCoordinator_.asyncRenderer().isBusy()) {
    window_.wakeEventLoop();
    return;
  }

  const std::vector<svg::SVGElement> previewElements = combinedSourcePreviewElements();
  const bool overlayChanged = renderCoordinator_.setSourceHoverElements(previewElements);
  const bool sourceChanged =
      textEditor_.setHoverSourceRanges(sourceHoverRangesForElements(previewElements));
  if (overlayChanged) {
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
        RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
        selectTool_.activeTransformBoundsPreview());
  }
  if (overlayChanged || sourceChanged) {
    window_.wakeEventLoop();
  }
}

void EditorShell::setReferenceHighlightChipHovered(bool hovered) {
  if (referenceHighlightChipHovered_ == hovered) {
    return;
  }

  referenceHighlightChipHovered_ = hovered;
  applyReferenceHighlightPreview();
}

std::optional<EditorShell::SelectionChipBounds> EditorShell::selectionChipBounds(
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) const {
  if (renderCoordinator_.selectionBoundsCache().lastSelection != app_.selectedElements() ||
      renderCoordinator_.selectionBoundsCache().displayedBoundsDoc.empty()) {
    return std::nullopt;
  }

  Box2d selectionBounds =
      CombinedSelectionBounds(renderCoordinator_.selectionBoundsCache().displayedBoundsDoc);
  Transform2d documentFromSelectionBoundsDocument;
  if (activeGesturePreview.has_value()) {
    selectionBounds = activeGesturePreview->startBoundsDoc;
    documentFromSelectionBoundsDocument = activeGesturePreview->documentFromStartDocument;
  }

  const Box2d transformedSelectionBoundsDoc =
      TransformDocumentBox(selectionBounds, documentFromSelectionBoundsDocument);
  const Box2d selectionScreenBounds =
      interactionController_.viewport().documentToScreen(transformedSelectionBoundsDoc);
  if (selectionScreenBounds.isEmpty()) {
    return std::nullopt;
  }

  Vector2d chipAnchorDoc = transformedSelectionBoundsDoc.topLeft;
  if (activeGesturePreview.has_value() &&
      activeGesturePreview->kind == SelectTool::ActiveGestureKind::Rotate) {
    chipAnchorDoc = documentFromSelectionBoundsDocument.transformPosition(
        activeGesturePreview->startBoundsDoc.topLeft);
  }

  return SelectionChipBounds{
      .documentBounds = transformedSelectionBoundsDoc,
      .screenBounds = selectionScreenBounds,
      .chipAnchorScreen = interactionController_.viewport().documentToScreen(chipAnchorDoc),
  };
}

std::optional<Box2d> EditorShell::selectionSizeChipScreenRect(
    std::string_view label, const Vector2d& chipAnchorScreen) const {
  if (label.empty()) {
    return std::nullopt;
  }

  const Box2d imageRect = interactionController_.viewport().imageScreenRect();
  const ImVec2 textSize = SelectionSizeChipTextSize(label, SelectionSizeChipFont(uiFontBold_));
  const double width = static_cast<double>(textSize.x + 2.0f * kSelectionSizeChipPaddingX);
  const double height = static_cast<double>(textSize.y + 2.0f * kSelectionSizeChipPaddingY);
  const double maxX = std::max(imageRect.topLeft.x, imageRect.bottomRight.x - width);
  const double x = std::clamp(chipAnchorScreen.x, imageRect.topLeft.x, maxX);
  double y = chipAnchorScreen.y - height - kSelectionSizeChipGapFromAabb;
  if (y < imageRect.topLeft.y) {
    y = chipAnchorScreen.y + kSelectionSizeChipGapFromAabb;
  }
  const double maxY = std::max(imageRect.topLeft.y, imageRect.bottomRight.y - height);
  y = std::clamp(y, imageRect.topLeft.y, maxY);
  return Box2d::FromXYWH(x, y, width, height);
}

void EditorShell::renderSelectionSizeChip(
    const SelectionTransformHandleIntent& hoverTransformIntent,
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
  const bool hoverResizeHandle = !activeGesturePreview.has_value() &&
                                 hoverTransformIntent.kind == SelectionTransformHandleKind::Resize;
  if (!activeGesturePreview.has_value() && !hoverResizeHandle) {
    return;
  }

  const std::optional<SelectionChipBounds> bounds = selectionChipBounds(activeGesturePreview);
  if (!bounds.has_value()) {
    return;
  }

  std::string label;
  if (!activeGesturePreview.has_value() ||
      activeGesturePreview->kind == SelectTool::ActiveGestureKind::Resize) {
    label = SelectionSizeChipLabel(bounds->screenBounds);
  } else if (activeGesturePreview->kind == SelectTool::ActiveGestureKind::Rotate) {
    label = SelectionAngleChipLabel(activeGesturePreview->documentFromStartDocument);
  } else {
    label = SelectionPositionChipLabel(bounds->documentBounds);
  }

  const std::optional<Box2d> rect = selectionSizeChipScreenRect(label, bounds->chipAnchorScreen);
  if (!rect.has_value()) {
    return;
  }

  ImFont* chipFont = SelectionSizeChipFont(uiFontBold_);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(
      ImVec2(static_cast<float>(rect->topLeft.x), static_cast<float>(rect->topLeft.y)),
      ImVec2(static_cast<float>(rect->bottomRight.x), static_cast<float>(rect->bottomRight.y)),
      IM_COL32(0, 111, 149, 255), kSelectionSizeChipRadius);
  drawList->AddText(chipFont, SelectionSizeChipFontSize(),
                    ImVec2(static_cast<float>(rect->topLeft.x) + kSelectionSizeChipPaddingX,
                           static_cast<float>(rect->topLeft.y) + kSelectionSizeChipPaddingY),
                    IM_COL32(255, 255, 255, 255), label.c_str(), label.c_str() + label.size());
}

std::optional<Box2d> EditorShell::referenceHighlightChipScreenRect(std::string_view label) const {
  if (label.empty() || referenceHighlightSummary_.totalCount() <= 1 ||
      renderCoordinator_.selectionBoundsCache().lastSelection != app_.selectedElements() ||
      renderCoordinator_.selectionBoundsCache().displayedBoundsDoc.empty()) {
    return std::nullopt;
  }

  const Box2d selectionBounds =
      CombinedSelectionBounds(renderCoordinator_.selectionBoundsCache().displayedBoundsDoc);
  const Vector2d topLeft =
      interactionController_.viewport().documentToScreen(selectionBounds.topLeft);
  const Box2d imageRect = interactionController_.viewport().imageScreenRect();
  const ImVec2 textSize = ReferenceHighlightChipTextSize(label);
  const double width = static_cast<double>(textSize.x + 2.0f * kReferenceChipPaddingX);
  const double height = static_cast<double>(textSize.y + 2.0f * kReferenceChipPaddingY);
  const double maxX = std::max(imageRect.topLeft.x, imageRect.bottomRight.x - width);
  const double x = std::clamp(topLeft.x, imageRect.topLeft.x, maxX);
  double y = topLeft.y - height - kReferenceChipGapFromAabb;
  if (y < imageRect.topLeft.y) {
    y = topLeft.y + kReferenceChipGapFromAabb;
  }
  const double maxY = std::max(imageRect.topLeft.y, imageRect.bottomRight.y - height);
  y = std::clamp(y, imageRect.topLeft.y, maxY);
  return Box2d::FromXYWH(x, y, width, height);
}

void EditorShell::renderReferenceHighlightChip() {
  if (selectTool_.activeTransformBoundsPreview().has_value()) {
    setReferenceHighlightChipHovered(false);
    return;
  }

  refreshReferenceHighlightSummaryIfNeeded();
  const std::string label = ReferenceHighlightChipLabel(referenceHighlightSummary_);
  const std::optional<Box2d> rect = referenceHighlightChipScreenRect(label);
  if (!rect.has_value()) {
    setReferenceHighlightChipHovered(false);
    return;
  }

  const ImVec2 mouse = ImGui::GetMousePos();
  const bool hovered = ContainsScreenPoint(*rect, mouse);
  setReferenceHighlightChipHovered(hovered);
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, /*repeat=*/false)) {
      referenceHighlightActive_ = !referenceHighlightActive_;
      applyReferenceHighlightPreview();
    }
  }

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImU32 bg = referenceHighlightActive_ ? IM_COL32(0, 135, 170, 245)
                   : hovered                 ? IM_COL32(48, 70, 78, 245)
                                             : IM_COL32(34, 48, 54, 235);
  drawList->AddRectFilled(
      ImVec2(static_cast<float>(rect->topLeft.x), static_cast<float>(rect->topLeft.y)),
      ImVec2(static_cast<float>(rect->bottomRight.x), static_cast<float>(rect->bottomRight.y)), bg,
      kReferenceChipRadius);
  drawList->AddText(ImGui::GetFont(), ReferenceHighlightChipFontSize(),
                    ImVec2(static_cast<float>(rect->topLeft.x) + kReferenceChipPaddingX,
                           static_cast<float>(rect->topLeft.y) + kReferenceChipPaddingY),
                    IM_COL32(255, 255, 255, 255), label.c_str(), label.c_str() + label.size());
}

void EditorShell::renderPenToolPreview() {
  if (activeTool_ != ActiveTool::Pen || !penTool_.isDrafting()) {
    return;
  }

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ViewportState& viewport = interactionController_.viewport();
  const ImU32 pathColor = ImGui::GetColorU32(ImVec4(0.10f, 0.43f, 1.0f, 1.0f));
  const ImU32 handleColor = ImGui::GetColorU32(ImVec4(0.10f, 0.43f, 1.0f, 0.42f));
  const ImU32 anchorFill = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  const ImU32 anchorStroke = ImGui::GetColorU32(ImVec4(0.06f, 0.20f, 0.48f, 1.0f));

  for (const PenTool::PreviewHandleLine& handle : penTool_.previewHandleLines()) {
    drawList->AddLine(ToImVec2(viewport.documentToScreen(handle.start)),
                      ToImVec2(viewport.documentToScreen(handle.end)), handleColor, 1.2f);
    drawList->AddCircleFilled(ToImVec2(viewport.documentToScreen(handle.end)), 3.0f, handleColor);
  }

  for (const PenTool::PreviewSegment& segment : penTool_.previewSegments()) {
    if (!segment.cubic) {
      drawList->AddLine(ToImVec2(viewport.documentToScreen(segment.start)),
                        ToImVec2(viewport.documentToScreen(segment.end)), pathColor, 2.0f);
      continue;
    }

    constexpr int kCubicSteps = 24;
    Vector2d previous = segment.start;
    for (int i = 1; i <= kCubicSteps; ++i) {
      const double t = static_cast<double>(i) / static_cast<double>(kCubicSteps);
      const Vector2d current =
          CubicPoint(segment.start, segment.control1, segment.control2, segment.end, t);
      drawList->AddLine(ToImVec2(viewport.documentToScreen(previous)),
                        ToImVec2(viewport.documentToScreen(current)), pathColor, 2.0f);
      previous = current;
    }
  }

  for (const Vector2d& anchor : penTool_.previewAnchors()) {
    const ImVec2 center = ToImVec2(viewport.documentToScreen(anchor));
    const ImVec2 min(center.x - 4.0f, center.y - 4.0f);
    const ImVec2 max(center.x + 4.0f, center.y + 4.0f);
    drawList->AddRectFilled(min, max, anchorFill, 1.0f);
    drawList->AddRect(min, max, anchorStroke, 1.0f, 0, 1.4f);
  }
}

void EditorShell::openRenderPaneContextMenu(const Vector2d& documentPoint) {
  renderContextMenuDocumentPoint_ = documentPoint;
  renderContextMenuHitElement_.reset();

  if (app_.hasDocument() && !renderCoordinator_.asyncRenderer().isBusy()) {
    if (std::optional<svg::SVGGeometryElement> hit = app_.hitTest(documentPoint)) {
      renderContextMenuHitElement_ = *hit;
    }
  }

  renderContextMenuOpenRequested_ = true;
  window_.wakeEventLoop();
}

void EditorShell::renderRenderPaneContextMenu() {
  if (renderContextMenuOpenRequested_) {
    ImGui::OpenPopup(kRenderPaneContextMenuName.data());
    renderContextMenuOpenRequested_ = false;
  }

  if (!ImGui::BeginPopup(kRenderPaneContextMenuName.data())) {
    return;
  }

  bool selectionChanged = false;
  const bool rendererBusy = renderCoordinator_.asyncRenderer().isBusy();
  if (!app_.hasDocument()) {
    ImGui::BeginDisabled();
    ImGui::MenuItem("No document loaded");
    ImGui::EndDisabled();
  } else if (rendererBusy && !renderContextMenuHitElement_.has_value()) {
    ImGui::BeginDisabled();
    ImGui::MenuItem("Renderer busy");
    ImGui::EndDisabled();
  } else if (renderContextMenuHitElement_.has_value()) {
    const svg::SVGElement hitElement = *renderContextMenuHitElement_;
    const std::string label = ElementContextMenuLabel(hitElement);
    ImGui::TextUnformatted(label.c_str(), label.c_str() + label.size());
    ImGui::Separator();

    const bool alreadySelected =
        ContainsElement(std::span<const svg::SVGElement>(app_.selectedElements()), hitElement);
    if (ImGui::MenuItem("Select Element")) {
      app_.setSelection(hitElement);
      selectionChanged = true;
    }
    if (ImGui::MenuItem("Add to Selection", nullptr, false, !alreadySelected)) {
      app_.addToSelection(hitElement);
      selectionChanged = true;
    }
  } else {
    ImGui::BeginDisabled();
    ImGui::MenuItem("No element under cursor");
    ImGui::EndDisabled();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Clear Selection", nullptr, false, app_.hasSelection())) {
    app_.clearSelection();
    selectionChanged = true;
  }
  if (ImGui::MenuItem("Delete Selection", nullptr, false, app_.hasSelection())) {
    selectionChanged = app_.deleteSelectionWithUndo(textEditor_.getText());
  }
  if (referenceHighlightSummary_.totalCount() > 1) {
    if (ImGui::MenuItem("Highlight Refs", nullptr, referenceHighlightActive_)) {
      referenceHighlightActive_ = !referenceHighlightActive_;
      applyReferenceHighlightPreview();
    }
  }
  if (ImGui::MenuItem("Show Source", nullptr, sourcePaneVisible_)) {
    setSourcePaneVisible(!sourcePaneVisible_);
  }
  if (ImGui::MenuItem("Source Focus Mode", nullptr, sourceFocusMode_)) {
    toggleSourceFocusMode();
  }

  if (selectionChanged) {
    referenceHighlightActive_ = false;
    referenceHighlightChipHovered_ = false;
    referenceHighlightSummary_ = ReferenceHighlightSummary{};
    lastReferenceHighlightSelection_.clear();
    updateSourceHoverPreview();
  }

  if (selectionChanged && !rendererBusy) {
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
        RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
        selectTool_.activeTransformBoundsPreview());
    window_.wakeEventLoop();
  }

  ImGui::EndPopup();
}

std::optional<StyleFocus> EditorShell::styleFocusAtSourceCursor() {
  if (!app_.hasDocument() || textEditor_.isTextChanged() || app_.document().hasPendingMutations()) {
    return std::nullopt;
  }
  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (textEditor_.getText() != documentSource) {
    return std::nullopt;
  }

  const std::size_t cursorOffset =
      textEditor_.getByteOffsetAtCoordinates(textEditor_.getCursorPosition());
  return styleFocusAtSourceOffset(cursorOffset);
}

void EditorShell::applyStyleFocus(StyleFocus styleFocus) {
  applySourcePartition(std::move(styleFocus.partition));
  sourceFocusOriginatedInStyle_ = true;
  sourceSelectionOriginatedInText_ = false;
  if (styleFocus.reverseReferenceExpansionSuppressed) {
    return;
  }
  if (app_.selectedElements() != styleFocus.impactedElements) {
    app_.setSelection(std::move(styleFocus.impactedElements));
    sourceSelectionOriginatedInText_ = app_.selectedElements() != lastHighlightedSelection_;
  }
}

void EditorShell::syncSelectionFromSourceCursorIfNeeded() {
  const bool sourceCursorNavigationActive =
      textEditor_.isFocused() || textEditor_.didMouseChangeCursorPosition();
  if (!textEditor_.isCursorPositionChanged() || !sourceCursorNavigationActive ||
      !app_.hasDocument() || textEditor_.isTextChanged() || textEditor_.hasSelection()) {
    return;
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (textEditor_.getText() != documentSource) {
    return;
  }

  std::optional<StyleFocus> styleFocus = styleFocusAtSourceCursor();
  if (styleFocus.has_value()) {
    applyStyleFocus(std::move(*styleFocus));
    window_.wakeEventLoop();
    return;
  }

  if (sourceFocusOriginatedInStyle_ && textEditor_.isCursorInsideFocusRange()) {
    return;
  }

  std::optional<svg::SVGElement> element =
      FindElementAtSourceCursor(app_.document().document(), textEditor_);
  if (!element.has_value()) {
    return;
  }

  if (app_.selectedElements().size() == 1u && app_.selectedElements().front() == *element) {
    sourceSelectionOriginatedInText_ = false;
    if (sourceFocusOriginatedInStyle_) {
      updateSourceFocusView(/*scrollToSelection=*/false);
      window_.wakeEventLoop();
    }
    return;
  }

  app_.setSelection(*element);
  sourceSelectionOriginatedInText_ = app_.selectedElements() != lastHighlightedSelection_;
  updateSourceFocusView(/*scrollToSelection=*/false);
  window_.wakeEventLoop();
}

void EditorShell::applySourcePartition(FocusPartition partition) {
  if (!sourceFocusMode_) {
    partition.fullColor.clear();
    partition.dimmed.clear();
    partition.hidden.clear();
  }

  textEditor_.setFocusPartition(partition);
}

void EditorShell::updateSourceFocusView(bool scrollToSelection) {
  sourceFocusOriginatedInStyle_ = false;
  if (!app_.hasDocument() || app_.selectedElements().empty()) {
    textEditor_.clearFocusPartition();
    return;
  }

  const svg::SVGElement selected = *app_.selectedElement();
  applySourcePartition(ComputeFocusPartition(app_.document().document(), app_.selectedElements()));
  if (scrollToSelection) {
    std::ignore = HighlightElementSource(textEditor_, selected);
  }
}

void EditorShell::updateSourceStyleDecorations() {
  const auto clearDecorations = [this]() {
    styleSourceContributions_.clear();
    styleSourceDecorationsValid_ = false;
    styleSourceDecorationSourceVersion_ = 0;
    styleSourceDecorationText_.clear();
    std::ignore = textEditor_.clearSourceStyleDecorations();
  };

  if (!app_.hasDocument() || textEditor_.isTextChanged() || app_.document().hasPendingMutations()) {
    clearDecorations();
    return;
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (documentSource.empty() || textEditor_.getText() != documentSource) {
    clearDecorations();
    return;
  }

  const std::uint64_t sourceVersion = app_.document().document().sourceVersion();
  if (styleSourceDecorationsValid_ && styleSourceDecorationSourceVersion_ == sourceVersion &&
      styleSourceDecorationText_ == documentSource) {
    return;
  }

  StyleSourceAnnotations annotations =
      ComputeStyleSourceAnnotations(app_.document().document(), documentSource);
  styleSourceContributions_ = std::move(annotations.contributions);

  std::vector<TextEditor::SourceStyleDecoration> decorations;
  decorations.reserve(styleSourceContributions_.size());
  for (const StyleSourceContribution& contribution : styleSourceContributions_) {
    decorations.push_back(TextEditor::SourceStyleDecoration{
        .id = contribution.id,
        .range = contribution.sourceRange,
        .chipRange = contribution.chipRange,
        .ineffective = !contribution.effective,
        .showChip = contribution.showChip,
        .chipCount = contribution.matchedElementCount,
        .showOverflowMarker = contribution.showOverflowMarker,
        .tooltip = contribution.tooltip,
        .chipTooltip = contribution.chipTooltip,
        .overflowTooltip = contribution.overflowTooltip,
    });
  }

  std::ignore = textEditor_.setSourceStyleDecorations(std::move(decorations));
  styleSourceDecorationsValid_ = true;
  styleSourceDecorationSourceVersion_ = sourceVersion;
  styleSourceDecorationText_ = documentSource;
}

void EditorShell::applySourceStyleDecorationChipClick() {
  const std::optional<std::size_t> clickedId = textEditor_.takeClickedSourceStyleChipId();
  if (!clickedId.has_value()) {
    return;
  }

  auto contributionIter =
      std::find_if(styleSourceContributions_.begin(), styleSourceContributions_.end(),
                   [clickedId](const StyleSourceContribution& contribution) {
                     return contribution.id == *clickedId;
                   });
  if (contributionIter == styleSourceContributions_.end()) {
    return;
  }
  if (contributionIter->showOverflowMarker) {
    return;
  }

  app_.setSelection(contributionIter->matchedElements);
  sourceSelectionOriginatedInText_ = true;
  sourceFocusOriginatedInStyle_ = false;
  referenceHighlightActive_ = false;
  referenceHighlightChipHovered_ = false;
  referenceHighlightSummary_ = ReferenceHighlightSummary{};
  lastReferenceHighlightSelection_.clear();
  updateSourceFocusView(/*scrollToSelection=*/false);

  if (!renderCoordinator_.asyncRenderer().isBusy()) {
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
        RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
        selectTool_.activeTransformBoundsPreview());
  }

  window_.wakeEventLoop();
}

void EditorShell::setSourceFocusMode(bool enabled) {
  sourceFocusMode_ = enabled;
  if (sourceFocusOriginatedInStyle_) {
    if (std::optional<StyleFocus> styleFocus = styleFocusAtSourceCursor()) {
      applyStyleFocus(std::move(*styleFocus));
      window_.wakeEventLoop();
      return;
    }
  }
  updateSourceFocusView(/*scrollToSelection=*/true);
  window_.wakeEventLoop();
}

void EditorShell::toggleSourceFocusMode() {
  setSourceFocusMode(!sourceFocusMode_);
}

void EditorShell::setSourcePaneVisible(bool visible) {
  if (sourcePaneVisible_ == visible) {
    return;
  }

  sourcePaneVisible_ = visible;
  if (!sourcePaneVisible_) {
    std::ignore = textEditor_.clearHoverSourceRanges();
    if (renderCoordinator_.setSourceHoverElements({}) &&
        !renderCoordinator_.asyncRenderer().isBusy()) {
      renderCoordinator_.rasterizeOverlayForCurrentSelection(
          app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
          RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
          selectTool_.activeTransformBoundsPreview());
    }
  }
  window_.wakeEventLoop();
}

void EditorShell::revealSourceRange(SourceByteRange byteRange) {
  setSourcePaneVisible(true);
  if (HighlightSourceByteRange(textEditor_, byteRange)) {
    textEditor_.flashSourceRange(byteRange);
  }
  window_.wakeEventLoop();
}

void EditorShell::runFrame() {
  ZoneScopedN("EditorShell::runFrame");
  textures_.advancePresentationFrame();
  layerInspectorPanel_.advancePresentationFrame();
  if (reproRecorder_) {
    // Snapshot before any widget consumes input events. ImGui's IO
    // state for the frame has been populated by
    // `ImGui_ImplGlfw_NewFrame` (called in `window_.beginFrame()`
    // upstream of `runFrame`); nothing below has touched it yet.
    //
    // The viewport snapshot is the OUTCOME of any previous frame's
    // viewport mutation (pinch-zoom, keyboard zoom, pan). Capturing
    // it every frame is what lets `RnrReplayTest`'s
    // `ApplyRecordedViewport` reconstruct zoom changes during
    // playback, even for gestures (macOS trackpad pinch via
    // `PinchEventMonitor`) that bypass ImGui's input boundary and
    // therefore aren't visible to the recorder as discrete events.
    const ViewportState& vp = interactionController_.viewport();
    repro::FrameContext frameContext;
    frameContext.viewport = repro::ReproViewport{
        .paneOriginX = vp.paneOrigin.x,
        .paneOriginY = vp.paneOrigin.y,
        .paneSizeW = vp.paneSize.x,
        .paneSizeH = vp.paneSize.y,
        .devicePixelRatio = vp.devicePixelRatio,
        .zoom = vp.zoom,
        .panDocX = vp.panDocPoint.x,
        .panDocY = vp.panDocPoint.y,
        .panScreenX = vp.panScreenPoint.x,
        .panScreenY = vp.panScreenPoint.y,
        .viewBoxX = vp.documentViewBox.topLeft.x,
        .viewBoxY = vp.documentViewBox.topLeft.y,
        .viewBoxW = vp.documentViewBox.size().x,
        .viewBoxH = vp.documentViewBox.size().y,
    };
    reproRecorder_->snapshotFrame(frameContext);
  }
  interactionController_.noteFrameDelta(ImGui::GetIO().DeltaTime * 1000.0f);
  updateWindowTitle();

  renderCoordinator_.pollRenderResult(app_, interactionController_.viewport(), textures_,
                                      &interactionController_.frameHistory());

  if (!renderCoordinator_.asyncRenderer().isBusy()) {
    if (app_.flushFrame()) {
      renderCoordinator_.refreshSelectionBoundsCache(app_);
    }
  }

  documentSyncController_.syncParseErrorMarkers(app_, textEditor_);
  documentSyncController_.applyPendingWritebacks(app_, selectTool_, textEditor_);

  const Vector2i windowSize = window_.windowSize();
  const float menuBarHeight = ImGui::GetFrameHeight();
  const float paneOriginY = menuBarHeight;
  const float paneHeight = std::max(0.0f, static_cast<float>(windowSize.y) - menuBarHeight);
  const EditorMainPaneLayout mainPaneLayout = ComputeEditorMainPaneLayout({
      .windowWidth = static_cast<float>(windowSize.x),
      .sourcePaneVisible = sourcePaneVisible_,
      .sourcePaneWidth = sourcePaneWidth_,
      .minSourcePaneWidth = kMinSourcePaneWidth,
      .maxSourcePaneWidth = kMaxSourcePaneWidth,
      .rightPaneWidth = rightPaneWidth_,
      .minRightPaneWidth = kMinRightPaneWidth,
      .maxRightPaneWidth = kMaxRightPaneWidth,
      .minRenderPaneWidth = kMinRightPaneWidth,
  });
  rightPaneWidth_ = mainPaneLayout.rightPaneWidth;
  const float rightPaneX = mainPaneLayout.rightPaneX;
  const float rightPaneGap = ImGui::GetStyle().ItemSpacing.y;
  const RightSidebarLayout rightSidebarLayout = ComputeRightSidebarLayout({
      .paneOriginY = paneOriginY,
      .paneHeight = paneHeight,
      .rightPaneGap = rightPaneGap,
      .treeViewHeightFraction = kTreeViewHeightFraction,
      .layerPanelHeightFraction = layerPanelHeightFraction_,
      .layerPanelDetached = layerPanelDetached_,
      .layerPanelSplitterThickness = kLayerPanelSplitterThickness,
      .minLayerPanelHeight = kMinLayerPanelHeight,
      .minInspectorPaneHeight = kMinInspectorPaneHeight,
  });
  layerPanelHeightFraction_ = rightSidebarLayout.layerPanelHeightFraction;
  const Vector2d renderPaneOrigin(mainPaneLayout.renderPaneX, paneOriginY);
  const Vector2d renderPaneSize(mainPaneLayout.renderPaneWidth, paneHeight);

  interactionController_.updatePaneLayout(
      renderPaneOrigin, renderPaneSize,
      app_.hasDocument() ? std::make_optional(ResolveDocumentViewBox(app_.document().document()))
                         : std::nullopt);

  handleGlobalShortcuts();

  MenuBarState menuState{
      .sourcePaneFocused = sourcePaneVisible_ && textEditor_.isFocused(),
      .canSave = app_.hasDocument(),
      .canRevert = app_.hasDocument() && app_.isDirty() && !app_.cleanSourceText().empty(),
      .canUndo = app_.canUndo(),
      .canRedo = app_.canRedo(),
      .sourceFocusMode = sourceFocusMode_,
  };
  const MenuBarActions menuActions = menuBarPresenter_.render(menuState, uiFontBold_);
  if (menuActions.openAbout) {
    dialogPresenter_.requestAbout();
  }
  if (menuActions.openFile) {
    dialogPresenter_.requestOpenFile(app_.currentFilePath());
  }
  if (menuActions.saveFile) {
    requestSave();
  }
  if (menuActions.saveFileAs) {
    requestSaveAs();
  }
  if (menuActions.revertFile) {
    requestRevert();
  }
  if (menuActions.quit) {
    glfwSetWindowShouldClose(window_.rawHandle(), GLFW_TRUE);
  }
  if (menuActions.undo && app_.canUndo()) {
    app_.undo();
  }
  if (menuActions.redo && app_.canRedo()) {
    app_.redo();
  }
  if (menuActions.cut) {
    textEditor_.cut();
  }
  if (menuActions.copy) {
    textEditor_.copy();
  }
  if (menuActions.paste) {
    textEditor_.paste();
  }
  if (menuActions.selectAll) {
    textEditor_.selectAll();
  }
  if (menuActions.zoomIn) {
    interactionController_.applyZoom(kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (menuActions.zoomOut) {
    interactionController_.applyZoom(1.0 / kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (menuActions.actualSize) {
    interactionController_.resetToActualSize();
  }
  if (menuActions.toggleSourceFocusMode) {
    toggleSourceFocusMode();
  }

  dialogPresenter_.render(
      [this](std::string_view path, std::string* error) { return tryOpenPath(path, error); },
      [this](std::string_view path, std::string* error) { return trySavePath(path, error); });

  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  std::ignore = highlightSelectionSourceIfNeeded();
  if (sourcePaneVisible_) {
    renderSourcePane(paneOriginY, paneHeight, mainPaneLayout.sourcePaneWidth, codeFont_);
  }
  renderRenderPane(renderPaneOrigin, renderPaneSize, kPaneFlags);
  renderSidebars(rightPaneX, rightPaneWidth_, paneOriginY, rightSidebarLayout, kPaneFlags);
  if (highlightSelectionSourceIfNeeded()) {
    window_.wakeEventLoop();
  }
  renderSourcePaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight,
                           mainPaneLayout.sourcePaneWidth);
  renderRightPaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight);
  if (!layerPanelDetached_) {
    renderLayerPanelSplitter(rightPaneX, rightPaneWidth_, rightSidebarLayout);
  }
  renderFloatingLayerPanel();
}

}  // namespace donner::editor
