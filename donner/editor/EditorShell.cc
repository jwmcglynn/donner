#define IMGUI_DEFINE_MATH_OPERATORS
#include "donner/editor/EditorShell.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "GLFW/glfw3.h"
#include "donner/base/RcString.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/DocumentSave.h"
#include "donner/editor/DragCoalesce.h"
#include "donner/editor/EditorShellPresentation.h"
#include "donner/editor/FocusView.h"
#include "donner/editor/FrameMissTelemetry.h"
#include "donner/editor/ImGuiClipboard.h"
#include "donner/editor/KeyboardShortcutPolicy.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/ShapeClipboardCommands.h"
#include "donner/editor/ShapeClipboardPayload.h"
#include "donner/editor/SourceSelection.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/StyleSourceAnnotations.h"
#include "donner/editor/TextToOutlines.h"
#include "donner/editor/ToolKeybinding.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/editor/ViewportSvgExport.h"
#include "donner/editor/XmlAutocomplete.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/repro/ReproRecorder.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererInterface.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif
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
constexpr double kSelectMarqueeHoldDelaySeconds = 0.20;
constexpr int kMaxSaveSyncFlushPasses = 4;
constexpr float kSelectionSizeChipPaddingX = 6.0f;
constexpr float kSelectionSizeChipPaddingY = 3.0f;
constexpr float kSelectionSizeChipRadius = 5.0f;

std::string PaintServerForDiagnostics(const svg::PaintServer& paint) {
  std::ostringstream stream;
  stream << paint;
  return stream.str();
}

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
    0x2726, 0x2726,  // Black four pointed star.
    0x2731, 0x2731,  // Heavy asterisk.
    0,
};
constexpr ImWchar kEditorSymbolGlyphRanges[] = {
    0x2217, 0x2217,  // Asterisk operator.
    0x2726, 0x2726,  // Black four pointed star.
    0x2731, 0x2731,  // Heavy asterisk.
    0,
};

bool ResourceDiagnosticsEnabled() {
  return std::getenv("DONNER_EDITOR_RESOURCE_LOG") != nullptr;
}

bool FrameMissTelemetryEnabled() {
  const char* value = std::getenv("DONNER_EDITOR_FRAME_MISS_LOG");
  if (value == nullptr) {
    return ResourceDiagnosticsEnabled();
  }

  const std::string_view valueView(value);
  return valueView != "0" && valueView != "false";
}

bool FrameMissTelemetryTargetIsStderr(std::string_view target) {
  return target.empty() || target == "1" || target == "true" || target == "stderr";
}

std::uint64_t MegabytesRoundedUp(std::uint64_t bytes) {
  constexpr std::uint64_t kBytesPerMiB = 1024u * 1024u;
  return (bytes + kBytesPerMiB - 1u) / kBytesPerMiB;
}

FrameMemorySample MemorySampleFromPresentationResources(
    const PresentationResourceStats& resources) {
  return FrameMemorySample{
      .overlayBytes = resources.overlayBytes,
      .activeTileBytes = resources.activeTileBytes,
      .overviewTileBytes = resources.overviewTileBytes,
      .retiredBytes = resources.pendingRetiredBytes + resources.agedRetiredBytes,
      .totalTrackedBytes = resources.totalTrackedBytes,
      .peakTrackedBytes = resources.peakTrackedBytes,
      .wgpuLifetimeTextureCreates = resources.wgpuLifetimeTextureCreates,
      .wgpuLifetimeBufferCreates = resources.wgpuLifetimeBufferCreates,
  };
}

FrameMissResourceTelemetry FrameMissTelemetryFromPresentationResources(
    const PresentationResourceStats& resources) {
  return FrameMissResourceTelemetry{
      .overlayBytes = resources.overlayBytes,
      .activeTileBytes = resources.activeTileBytes,
      .overviewTileBytes = resources.overviewTileBytes,
      .retiredBytes = resources.pendingRetiredBytes + resources.agedRetiredBytes,
      .totalTrackedBytes = resources.totalTrackedBytes,
      .peakTrackedBytes = resources.peakTrackedBytes,
      .wgpuLifetimeTextureCreates = resources.wgpuLifetimeTextureCreates,
      .wgpuLifetimeBufferCreates = resources.wgpuLifetimeBufferCreates,
  };
}

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

// Build the "<label> (<key>)" tooltip string for a tool button from the shared
// keybinding table, so the tooltip and the keyboard shortcut handler can never
// disagree about which key activates the tool.
std::string ToolTooltipText(ToolId tool) {
  const ToolKeybinding binding = KeybindingForTool(tool);
  return std::string(binding.label) + " (" + binding.key + ")";
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
  const css::Color color = element.getComputedStyle().color.get().value();
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

  return document.withReadAccess(
      [source, &reference](svg::DocumentReadAccess& access) -> std::optional<SourceByteRange> {
        const std::optional<svg::ResolvedReference> resolved = reference.resolve(access.registry());
        if (!resolved.has_value() || !resolved->valid()) {
          return std::nullopt;
        }

        return EntitySourceByteRange(resolved->handle, source);
      });
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
      attrName == "fill" ? style.fill.get().value() : style.stroke.get().value();
  ToolbarPaintSlotState state =
      ToolbarPaintSlotStateForPaintServer(paint, CurrentColorForElement(element), document, source);

  if (state.isCustom || !state.isNone) {
    return state;
  }

  const std::optional<RcString> attribute =
      element.withReadAccess([&element, attrName](svg::DocumentReadAccess&, EntityHandle) {
        return element.getAttribute(xml::XMLQualifiedNameRef(attrName));
      });
  if (attribute.has_value() && std::string_view(*attribute) != "none") {
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
    if (std::ranges::find(*target, element) == target->end()) {
      target->push_back(element);
    }
  }
}

bool ContainsElement(std::span<const svg::SVGElement> elements, const svg::SVGElement& element) {
  return std::ranges::find(elements, element) != elements.end();
}

std::string ElementContextMenuLabel(const svg::SVGElement& element) {
  return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
    const std::string tagName(element.tagName().name);
    std::string label = "<";
    label.append(tagName);
    label.push_back('>');

    const RcString id = element.id();
    const std::string_view idSv = id;
    if (!idSv.empty()) {
      label.push_back(' ');
      label.push_back('#');
      label.append(idSv.data(), idSv.size());
    }

    return label;
  });
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
  const std::optional<Box2d> viewBox = document.withReadAccess(
      [&document](svg::DocumentReadAccess&) { return document.svgElement().viewBox(); });
  if (viewBox.has_value()) {
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
      thumbnailTextures_(window.geodeDevice()),
      layerThumbnailRenderer_(window.geodeDevice()),
      renderCoordinator_(window.geodeDevice()),
      rotateCursorSet_(),
      documentSyncController_(InitialDocumentSyncSource(options_)),
      interactionController_(),
      inputBridge_(window_, kWheelZoomStep),
      compositorDebugPanel_(window.geodeDevice()),
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
  shapeClipboard_ = std::make_unique<ImGuiClipboard>();
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
#ifdef DONNER_EDITOR_WGPU
  if (window_.geodeFramebufferDevice() != nullptr) {
    directCheckerboardRenderer_ =
        std::make_unique<FramebufferCheckerboardRenderer>(window_.geodeFramebufferDevice());
    directDocumentRenderer_ =
        std::make_unique<svg::RendererGeode>(window_.geodeFramebufferDevice());
    directOverlayRenderer_ = std::make_unique<svg::RendererGeode>(window_.geodeFramebufferDevice());
  }
#endif
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
#ifdef DONNER_EDITOR_WGPU
  window_.setWgpuDirectRenderCallback({});
#endif
  if (reproRecorder_) {
    if (!reproRecorder_->flush()) {
      std::fprintf(stderr, "[repro] flush failed — recording lost\n");
    }
  }
}

void EditorShell::overrideViewportForReplay(const ViewportState& viewport) {
  pendingViewportReplayOverride_ = viewport;
}

void EditorShell::queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput input) {
  pendingDocumentSpaceReplayInput_ = std::move(input);
}

void EditorShell::queueScrollEventForReplayForTesting(RenderPaneScrollEvent event) {
  inputBridge_.events().push_back(event);
}

void EditorShell::applyReplayActionForTesting(const repro::ReproAction& action) {
  switch (action.kind) {
    case repro::ReproAction::Kind::SetActiveTool:
      if (action.tool == "select") {
        if (penTool_.commitOpenPath(app_)) {
          flushQueuedMutationAndRefreshOverlay();
        }
        activeTool_ = ActiveTool::Select;
      } else if (action.tool == "pen") {
        activeTool_ = ActiveTool::Pen;
      } else if (action.tool == "text") {
        activeTool_ = ActiveTool::Text;
      }
      break;
    case repro::ReproAction::Kind::SetStyleProperty:
      if (action.propertyName == "fill") {
        app_.setActiveFill(action.propertyValue);
      } else if (action.propertyName == "stroke") {
        app_.setActiveStroke(action.propertyValue);
      } else if (action.propertyName == "stroke-width") {
        char* end = nullptr;
        const double strokeWidth = std::strtod(action.propertyValue.c_str(), &end);
        if (end != action.propertyValue.c_str()) {
          app_.setActiveStrokeWidth(strokeWidth);
        }
      }
      if (app_.setStylePropertyOnSelection(action.propertyName, action.propertyValue)) {
        flushQueuedMutationAndRefreshOverlay();
      } else {
        window_.wakeEventLoop();
      }
      break;
    case repro::ReproAction::Kind::CommitPenPath:
      if (penTool_.commitOpenPath(app_)) {
        flushQueuedMutationAndRefreshOverlay();
      }
      break;
  }
}

std::optional<std::string> EditorShell::selectedElementLabelForReadback() const {
  const std::optional<svg::SVGElement>& selected = app_.selectedElement();
  if (!selected.has_value()) {
    return std::nullopt;
  }

  return ElementContextMenuLabel(*selected);
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
  FrameCostBreakdown frameCost = latestFrameCostForReadback_;
  const AsyncSVGDocument::FlushResult& lastFlush = app_.document().lastFlushResult();
  LayerInspectorStatusReadback readback{
      .canvasFreshness = freshness,
      .statusSuffix = std::string(CanvasFreshnessStatusSuffix(freshness)),
      .viewportDesiredCanvas = viewportDesiredCanvas,
      .documentCanvas = documentCanvas,
      .compositorCanvas = compositorCanvas,
      .metadataOnlyMissCount = textures_.metadataOnlyMissCount(),
      .duplicateLiveTextureCount = textures_.duplicateLiveTextureCount(),
      .documentFrameVersion = app_.document().currentFrameVersion(),
      .displayedDocVersion = renderCoordinator_.displayedDocVersionForDiagnostics(),
      .immediateOverlayDocumentVersion =
          renderCoordinator_.immediateOverlayDocumentVersionForDiagnostics(),
      .selectedCompositedEntity =
          renderCoordinator_.selectedCompositedEntityForDiagnostics(const_cast<EditorApp&>(app_)),
      .lastFlushAppliedCommands = lastFlush.appliedCommands,
      .lastFlushReplacedDocument = lastFlush.replacedDocument,
      .lastFlushRemovedElements = lastFlush.removedElements,
      .lastFlushCacheInvalidatedElements = lastFlush.cacheInvalidatedElements,
      .requestRenderAtEndOfFrame = requestRenderAtEndOfFrame_,
      .pendingSelectedLayerRasterizationEntity =
          renderCoordinator_.pendingSelectedLayerRasterizationEntityForDiagnostics(),
      .pendingSelectedLayerRasterizationVersion =
          renderCoordinator_.pendingSelectedLayerRasterizationVersionForDiagnostics(),
      .presentationResources = textures_.presentationResourceStats(),
      .frameCost = frameCost,
  };
  const std::optional<SelectTool::ActiveDragPreview> liveActiveDragPreview =
      selectTool_.activeDragPreview();
  const std::optional<SelectTool::ActiveDragPreview> activeDragPreview =
      renderCoordinator_.compositedPresentation().activePreviewForPresentation(
          liveActiveDragPreview);
  const std::optional<SelectTool::ActiveDragPreview> displayedDragPreview =
      renderCoordinator_.compositedPresentation().presentationPreview(activeDragPreview);
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromDragPreviews(activeDragPreview, displayedDragPreview);
  if (const std::optional<svg::SVGElement>& selected = app_.selectedElement();
      selected.has_value()) {
    if (const std::optional<RcString> style = selected->getAttribute("style"); style.has_value()) {
      readback.selectedStyleAttribute = std::string(std::string_view(*style));
    }
    if (const svg::PropertyRegistry* specifiedStyle = selected->specifiedStyle()) {
      if (const std::optional<svg::PaintServer> fill = specifiedStyle->fill.get();
          fill.has_value()) {
        readback.selectedLocalStyleFill = PaintServerForDiagnostics(*fill);
      }
    }
    if (const svg::PropertyRegistry* computedStyle = selected->computedStyleIfPresent()) {
      if (const std::optional<svg::PaintServer> fill = computedStyle->fill.get();
          fill.has_value()) {
        readback.selectedComputedFill = PaintServerForDiagnostics(*fill);
      }
    }
    if (const std::optional<svg::PaintServer> resolvedFill = selected->resolvedFillPaint();
        resolvedFill.has_value()) {
      readback.selectedRenderingInstanceFill = PaintServerForDiagnostics(*resolvedFill);
    }
    if (const std::optional<RcString> pathData = selected->getAttribute("d");
        pathData.has_value()) {
      readback.selectedPathDataAttribute = std::string(std::string_view(*pathData));
    }
  }
  readback.activeDragPreview = activeDragPreview;
  readback.displayedDragPreview = displayedDragPreview;
  readback.tiles.reserve(textures_.tiles().size());
  for (const GlTextureCache::TileView& tile : textures_.tiles()) {
    Vector2d presentedDragTranslationDoc = tile.dragTranslationDoc;
    if (tile.isDragTarget && activeDragPreview.has_value() && displayedDragPreview.has_value() &&
        activeDragPreview->entity == displayedDragPreview->entity) {
      presentedDragTranslationDoc +=
          activeDragPreview->translation - displayedDragPreview->translation;
    }
    const PresentedFrameTileGeometry tileGeometry =
        PresentedGeometryFromTileView(tile, activeDragPreview);
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
        .documentFromCachedDocument = tile.documentFromCachedDocument,
        .presentedDocumentFromCachedDocument =
            ResolvePresentedTileDocumentTransform(tileGeometry, dragBaseline),
        .textureHandle = static_cast<std::uint64_t>(tile.texture),
        .textureSnapshot = tile.textureSnapshot,
        .metadataOnly = tile.metadataOnly,
        .isDragTarget = tile.isDragTarget,
    });
  }
  readback.rowThumbnails.reserve(layersPanel_.rows().size());
  for (const LayerTreeRow& row : layersPanel_.rows()) {
    Vector2i bitmapDims = Vector2i::Zero();
    if (const svg::RendererBitmap* thumbnail = layersPanel_.rowThumbnail(row.stableId);
        thumbnail != nullptr) {
      bitmapDims = thumbnail->dimensions;
    }
    readback.rowThumbnails.push_back(LayerInspectorStatusReadback::RowThumbnail{
        .displayName = row.displayName,
        .stableId = row.stableId,
        .bitmapDimsPx = bitmapDims,
    });
  }
  const LayersPanel::ThumbnailRefreshStats& thumbnailStats = layersPanel_.thumbnailRefreshStats();
  readback.thumbnailRefreshStats = LayerInspectorStatusReadback::ThumbnailRefreshStats{
      .documentFrameVersion = thumbnailStats.documentFrameVersion,
      .rowCount = thumbnailStats.rowCount,
      .renderedCount = thumbnailStats.renderedCount,
      .reusedCount = thumbnailStats.reusedCount,
      .skippedForCanvasInvalidationCount = thumbnailStats.skippedForCanvasInvalidationCount,
      .renderMs = thumbnailStats.renderMs,
  };
  return readback;
}

void EditorShell::applyPendingDocumentSpaceReplayInputForTesting() {
  if (!pendingDocumentSpaceReplayInput_.has_value()) {
    return;
  }

  EditorShellDocumentReplayInput input = std::move(*pendingDocumentSpaceReplayInput_);
  pendingDocumentSpaceReplayInput_.reset();
  if (!app_.hasDocument()) {
    return;
  }

  if (input.hitElementId.has_value() && !input.hitElementId->empty()) {
    const std::optional<svg::SVGElement>& selected = app_.selectedElement();
    const bool selectionMatchesHit =
        selected.has_value() && selected->id() == input.hitElementId->c_str();
    if (!selectionMatchesHit) {
      if (std::optional<svg::SVGElement> element =
              app_.document().document().querySelector("#" + *input.hitElementId)) {
        app_.setSelection(*element);
        app_.flushFrame();
        renderCoordinator_.refreshSelectionBoundsCache(app_);
      }
    }
  }

  const auto rasterizeCurrentSelection = [&]() {
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    requestRenderAtEndOfFrame_ = true;
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
  };

  if (input.leftMousePressed) {
    lastPostedScreenPoint_.reset();
    if (activeTool_ == ActiveTool::Pen) {
      penTool_.onMouseDown(app_, input.documentPoint, input.modifiers);
      flushQueuedMutationAndRefreshOverlay();
    } else if (activeTool_ == ActiveTool::Text) {
      textTool_.onMouseDown(app_, input.documentPoint, input.modifiers);
      activeTool_ = ActiveTool::Select;
      flushQueuedMutationAndRefreshOverlay();
    } else {
      selectTool_.onMouseDown(app_, input.documentPoint, input.modifiers);
      rasterizeCurrentSelection();
    }
  } else if (input.leftMouseDown && activeTool_ == ActiveTool::Pen && penTool_.isDraggingAnchor()) {
    penTool_.onMouseMove(app_, input.documentPoint, /*buttonHeld=*/true, input.modifiers);
    if (app_.document().hasPendingMutations() && flushQueuedMutationAndRefreshOverlay()) {
      penDragFlushedThisFrame_ = true;
    }
  } else if (input.leftMouseDown && (selectTool_.isDragging() || selectTool_.isMarqueeing())) {
    selectTool_.onMouseMove(app_, input.documentPoint, /*buttonHeld=*/true, input.modifiers);
    if (!renderCoordinator_.asyncRenderer().isBusy()) {
      app_.flushFrame();
    }
    rasterizeCurrentSelection();
  }

  if (input.leftMouseReleased) {
    if (activeTool_ == ActiveTool::Pen) {
      penTool_.onMouseUp(app_, input.documentPoint);
      lastPostedScreenPoint_.reset();
      flushQueuedMutationAndRefreshOverlay();
      return;
    }

    const auto previewBeforeRelease = selectTool_.activeDragPreview();
    const bool previewHadVisualChange =
        previewBeforeRelease.has_value() &&
        (!previewBeforeRelease->documentFromCachedDocument.isIdentity() ||
         previewBeforeRelease->translation != Vector2d::Zero());
    selectTool_.onMouseUp(app_, input.documentPoint);
    lastPostedScreenPoint_.reset();
    if (previewBeforeRelease.has_value() && previewHadVisualChange) {
      const std::uint64_t settleTargetVersion = PostReleaseSettleTargetVersion(
          app_.document().currentFrameVersion(), app_.document().hasPendingMutations());
      renderCoordinator_.compositedPresentation().beginSettling(previewBeforeRelease,
                                                                settleTargetVersion);
    }
    if (!renderCoordinator_.asyncRenderer().isBusy()) {
      app_.flushFrame();
    }
    rasterizeCurrentSelection();
  }

  // Pen hover chrome for document-space input (replay and thin-client paths;
  // the live editor computes the same thing from the ImGui pointer in
  // renderRenderPane, which this later-queued input overrides).
  if (activeTool_ == ActiveTool::Pen && penTool_.isDrafting() && !input.leftMouseDown &&
      !penTool_.isDraggingAnchor()) {
    renderCoordinator_.setPenHoverChrome(
        penTool_.previewSegmentPath(input.documentPoint, input.modifiers),
        penTool_.wouldCloseAt(input.documentPoint, input.modifiers)
            ? std::make_optional(penTool_.draftStartPoint())
            : std::nullopt);
  }
}

void EditorShell::maybeLogResourceDiagnostics(const FrameCostBreakdown& frameCost) {
  if (!ResourceDiagnosticsEnabled()) {
    return;
  }

  ++resourceDiagnosticsFrame_;
  const PresentationResourceStats resources = textures_.presentationResourceStats();
  const bool firstFrame = resourceDiagnosticsFrame_ == 1;
  const bool periodic = resourceDiagnosticsFrame_ % 60 == 0;
  constexpr std::uint64_t kPeakLogStepBytes = 16u * 1024u * 1024u;
  const bool peakAdvanced =
      resources.peakTrackedBytes >= lastLoggedPresentationPeakBytes_ + kPeakLogStepBytes;
  if (!firstFrame && !periodic && !peakAdvanced) {
    return;
  }

  const std::uint64_t textureCreateDelta =
      resources.wgpuLifetimeTextureCreates - lastLoggedWgpuTextureCreates_;
  const std::uint64_t bufferCreateDelta =
      resources.wgpuLifetimeBufferCreates - lastLoggedWgpuBufferCreates_;
  lastLoggedWgpuTextureCreates_ = resources.wgpuLifetimeTextureCreates;
  lastLoggedWgpuBufferCreates_ = resources.wgpuLifetimeBufferCreates;
  lastLoggedPresentationPeakBytes_ =
      std::max(lastLoggedPresentationPeakBytes_, resources.peakTrackedBytes);
  std::cerr << "[DonnerResource] frame=" << resourceDiagnosticsFrame_
            << " tracked_mib=" << MegabytesRoundedUp(resources.totalTrackedBytes)
            << " peak_mib=" << MegabytesRoundedUp(resources.peakTrackedBytes)
            << " overlay_mib=" << MegabytesRoundedUp(resources.overlayBytes)
            << " active_tile_mib=" << MegabytesRoundedUp(resources.activeTileBytes)
            << " overview_tile_mib=" << MegabytesRoundedUp(resources.overviewTileBytes)
            << " retired_mib="
            << MegabytesRoundedUp(resources.pendingRetiredBytes + resources.agedRetiredBytes)
            << " active_tiles=" << resources.activeTileTextures
            << " overview_tiles=" << resources.overviewTileTextures << " retired_textures="
            << resources.pendingRetiredTextures + resources.agedRetiredTextures
            << " largest_allocation_px=" << resources.largestAllocationPx.x << "x"
            << resources.largestAllocationPx.y
            << " wgpu_texture_creates=" << resources.wgpuLifetimeTextureCreates
            << " wgpu_texture_creates_delta=" << textureCreateDelta
            << " wgpu_buffer_creates=" << resources.wgpuLifetimeBufferCreates
            << " wgpu_buffer_creates_delta=" << bufferCreateDelta
            << " overlay_upload_bytes=" << frameCost.overlay.payloadBytes
            << " tile_upload_bytes=" << frameCost.compositedUpload.payloadBytes << "\n";
}

void EditorShell::maybeLogFrameMissTelemetry(const FrameCostBreakdown& frameCost) {
  if (!FrameMissTelemetryEnabled()) {
    return;
  }

  const PresentationResourceStats resources = textures_.presentationResourceStats();
  const FrameMissTelemetryInput input{
      .frameIndex = frameTelemetryFrame_,
      .frameMs = interactionController_.frameHistory().latest(),
      .backendMs = interactionController_.frameHistory().latestBackend(),
      .frameCost = frameCost,
      .resources = FrameMissTelemetryFromPresentationResources(resources),
  };
  const std::string json = BuildFrameMissTelemetryJson(input);
  if (json.empty()) {
    return;
  }

  const char* targetValue = std::getenv("DONNER_EDITOR_FRAME_MISS_LOG");
  const std::string_view target =
      targetValue != nullptr ? std::string_view(targetValue) : std::string_view();
  if (targetValue != nullptr && !FrameMissTelemetryTargetIsStderr(target)) {
    std::ofstream output(std::string(target), std::ios::app);
    if (output.good()) {
      output << json;
      return;
    }

    if (!frameMissTelemetryWriteErrorLogged_) {
      std::cerr << "[DonnerFrameMiss] failed to open telemetry path: " << target << "\n";
      frameMissTelemetryWriteErrorLogged_ = true;
    }
  }

  std::cerr << "[DonnerFrameMiss] " << json;
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
  pendingViewportExport_ = false;
  dialogPresenter_.requestSaveFile(app_.currentFilePath(), std::move(error));
}

void EditorShell::requestExportViewportSvg(bool includeOverlay, std::string error) {
  pendingViewportExport_ = true;
  pendingViewportExportOverlay_ = includeOverlay;
  // Default export filename: "<stem>_viewport.svg" beside the source document,
  // or "untitled_viewport.svg" for documents with no path.
  std::string defaultPath;
  if (app_.currentFilePath().has_value()) {
    const std::filesystem::path sourcePath(*app_.currentFilePath());
    std::filesystem::path exportPath = sourcePath;
    exportPath.replace_filename(sourcePath.stem().string() + "_viewport.svg");
    defaultPath = exportPath.string();
  } else {
    defaultPath = "untitled_viewport.svg";
  }
  dialogPresenter_.requestSaveFile(std::make_optional(defaultPath), std::move(error));
}

bool EditorShell::tryExportViewportSvgToPath(std::string_view path, std::string* error) {
  if (!app_.hasDocument()) {
    *error = "No document is open to export.";
    return false;
  }
  if (!synchronizeSourceBeforeSave(error)) {
    return false;
  }

  const ViewportState& viewport = interactionController_.viewport();
  // The render pane content rect in screen (CSS) pixels, derived from the
  // viewport's pane origin/size. ViewportState is the sole crop/scale source.
  const Vector2d paneOrigin = viewport.paneOrigin;
  const Vector2d paneSize = viewport.paneSize;
  const Recti renderPaneRect(Vector2i(static_cast<int>(std::lround(paneOrigin.x)),
                                      static_cast<int>(std::lround(paneOrigin.y))),
                             Vector2i(static_cast<int>(std::lround(paneOrigin.x + paneSize.x)),
                                      static_cast<int>(std::lround(paneOrigin.y + paneSize.y))));

  ViewportExportOptions options;
  options.transparentBackground = true;
  options.includeSelectionOverlay = pendingViewportExportOverlay_;

  // Capture the overlay snapshot at export time so the serialized chrome samples
  // the same selection state the editor currently displays — it cannot be a
  // frame behind. Mirrors how `OverlayRenderer::drawChrome(renderer, editor)`
  // derives the transform + selection for the live chrome.
  std::optional<SelectionChromeSnapshot> overlaySnapshot;
  if (pendingViewportExportOverlay_) {
    const Transform2d canvasFromDocument = app_.document().document().canvasFromDocumentTransform();
    overlaySnapshot = OverlayRenderer::captureChromeSnapshot(
        app_.selectedElements(), /*marqueeRectDoc=*/std::nullopt, canvasFromDocument, std::nullopt,
        std::span<const svg::SVGElement>(), std::nullopt, selectionChromeDetailForActiveTool());
  }

  Result<std::string, std::string> exportResult =
      ExportViewportAsSvg(app_.document().document(), viewport, renderPaneRect, options,
                          overlaySnapshot.has_value() ? &*overlaySnapshot : nullptr);
  if (!exportResult.ok()) {
    *error = exportResult.error;
    return false;
  }

  const DocumentSaveResult saveResult =
      SaveSourceToPath(std::filesystem::path(std::string(path)), exportResult.value);
  if (!saveResult.ok()) {
    *error = saveResult.message;
    return false;
  }

  dialogPresenter_.clearSaveFileError();
  pendingViewportExport_ = false;
  pendingViewportExportOverlay_ = false;
  return true;
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
    if (interactionController_.applyZoom(kKeyboardZoomStep,
                                         interactionController_.viewport().paneCenter())) {
      requestRenderAtEndOfFrame_ = true;
    }
  }
  if (!anyPopupOpen && cmd &&
      (ImGui::IsKeyPressed(ImGuiKey_Minus, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, /*repeat=*/false))) {
    if (interactionController_.applyZoom(1.0 / kKeyboardZoomStep,
                                         interactionController_.viewport().paneCenter())) {
      requestRenderAtEndOfFrame_ = true;
    }
  }
  if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_0, /*repeat=*/false)) {
    if (interactionController_.resetToActualSize()) {
      requestRenderAtEndOfFrame_ = true;
    }
  }

  // Paint-order (z-order) shortcuts on the selected element: Cmd+] forward,
  // Cmd+[ backward, Cmd+Shift+] to front, Cmd+Shift+[ to back. The reorder is a
  // DOM move; the structured-editing reflection rewrites the source, and the
  // main-loop flush records the undo entry.
  if (!sourcePaneFocused && !anyPopupOpen && cmd) {
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, /*repeat=*/false)) {
      std::ignore = app_.reorderSelectedElement(shift ? EditorApp::ZOrder::BringToFront
                                                      : EditorApp::ZOrder::BringForward);
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, /*repeat=*/false)) {
      std::ignore = app_.reorderSelectedElement(shift ? EditorApp::ZOrder::SendToBack
                                                      : EditorApp::ZOrder::SendBackward);
    }
  }

  if (CanToggleSourceFocusModeFromShortcut(pressedEnter, cmd, anyPopupOpen)) {
    toggleSourceFocusMode();
  }

  if (!sourcePaneFocused && !anyPopupOpen && !cmd &&
      ImGui::IsKeyPressed(ImGuiKey_V, /*repeat=*/false)) {
    // Switching to Select commits any in-progress pen path as one undoable
    // command instead of discarding it.
    penTool_.commitOpenPath(app_);
    flushQueuedMutationAndRefreshOverlay();
    activeTool_ = ActiveTool::Select;
    textTool_.cancel();
  }

  if (!sourcePaneFocused && !anyPopupOpen && !cmd &&
      ImGui::IsKeyPressed(ImGuiKey_P, /*repeat=*/false)) {
    activeTool_ = ActiveTool::Pen;
  }

  if (!anyPopupOpen && !cmd && activeTool_ == ActiveTool::Pen && penTool_.isDrafting() &&
      (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false))) {
    // Enter commits the in-progress open path without closing it.
    penTool_.commitOpenPath(app_);
    flushQueuedMutationAndRefreshOverlay();
    return;
  }

  if (!sourcePaneFocused && !anyPopupOpen && !cmd &&
      ImGui::IsKeyPressed(ImGuiKey_T, /*repeat=*/false)) {
    activeTool_ = ActiveTool::Text;
  }

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      penTool_.isDrafting()) {
    // Escape ends the pen session, committing the placed anchors as an open
    // path (one undoable operation, same as Enter) — undo is the discard
    // mechanism. A draft with fewer than two anchors has no committable
    // segment, so it is discarded, restoring the document and undo stack to
    // the pre-pen baseline.
    if (!penTool_.commitOpenPath(app_)) {
      penTool_.cancel(app_);
    }
    flushQueuedMutationAndRefreshOverlay();
    return;
  }

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      activeTool_ == ActiveTool::Text) {
    textTool_.cancel();
    activeTool_ = ActiveTool::Select;
    return;
  }

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      app_.hasSelection()) {
    app_.setSelection(std::nullopt);
  }

  // Cmd+A ("Select All") is focus-aware. When the source pane owns keyboard focus it selects all
  // text in the XML editor; otherwise it selects every selectable canvas element via the same
  // setSelection() path normal canvas selection uses (so the canvas highlight, source-pane sync,
  // and overlay all update together). The absence of Shift keeps both branches distinct from the
  // Cmd+Shift+A "Deselect All" chord below.
  const bool pressedA = ImGui::IsKeyPressed(ImGuiKey_A, /*repeat=*/false);
  if (CanSelectAllFromSourcePaneShortcut(pressedA, cmd, shift, anyPopupOpen, sourcePaneFocused)) {
    textEditor_.selectAll();
  } else if (CanSelectAllFromCanvasShortcut(
                 pressedA, cmd, shift, anyPopupOpen, sourcePaneFocused,
                 /*canvasHasSelectableElements=*/canvasHasSelectableElements())) {
    selectAllCanvasElements();
  }

  // Cmd+Shift+A ("Deselect All") is focus-aware too. When the source pane owns keyboard focus it
  // collapses the text selection to the caret; otherwise it clears the canvas selection through the
  // same canonical clear path Escape uses (clearSelection() == setSelection(nullopt)) so the canvas
  // highlight, source-pane sync, and overlay all update together. The Shift requirement keeps both
  // branches distinct from a plain Cmd+A.
  if (CanDeselectAllFromSourcePaneShortcut(pressedA, cmd, shift, anyPopupOpen, sourcePaneFocused)) {
    textEditor_.clearSelection();
  } else if (CanDeselectAllFromCanvasShortcut(pressedA, cmd, shift, app_.hasSelection(),
                                              anyPopupOpen, sourcePaneFocused)) {
    app_.clearSelection();
  }

  // Shape clipboard shortcuts route through the canvas selection only when the
  // source pane is not focused — the text editor owns Cmd+X/C/V while it has
  // keyboard focus. Cmd+F is Paste in Front (exact-position duplication).
  if (!sourcePaneFocused && !anyPopupOpen && cmd && !shift) {
    if (ImGui::IsKeyPressed(ImGuiKey_X, /*repeat=*/false)) {
      cutSelectedShapesToClipboard();
    } else if (ImGui::IsKeyPressed(ImGuiKey_C, /*repeat=*/false)) {
      copySelectedShapesToClipboard();
    } else if (ImGui::IsKeyPressed(ImGuiKey_V, /*repeat=*/false)) {
      pasteShapesFromClipboard(/*inFront=*/false);
    } else if (ImGui::IsKeyPressed(ImGuiKey_F, /*repeat=*/false)) {
      pasteShapesFromClipboard(/*inFront=*/true);
    }
  }

  const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
  // While the Pen tool is drafting, Backspace/Delete removes the last placed
  // anchor (a lone-anchor draft is discarded entirely) — the draft path is the
  // selection, so falling through to delete-selection would nuke the whole
  // in-progress path.
  if (deleteKey && !anyPopupOpen && !sourcePaneFocused && activeTool_ == ActiveTool::Pen &&
      penTool_.isDrafting()) {
    if (penTool_.removeLastAnchor(app_)) {
      flushQueuedMutationAndRefreshOverlay();
    }
    return;
  }
  if (CanDeleteSelectedElementsFromShortcut(deleteKey, app_.hasSelection(), anyPopupOpen,
                                            sourcePaneFocused)) {
    std::ignore = app_.deleteSelectionWithUndo(textEditor_.getText());
  }
}

bool EditorShell::canvasHasSelectableElements() {
  if (!app_.hasDocument()) {
    return false;
  }
  return !app_.selectableElements().empty();
}

void EditorShell::selectAllCanvasElements() {
  if (!app_.hasDocument()) {
    return;
  }
  // Route through the same setSelection() path normal selection uses so the canvas highlight,
  // source-pane sync, and overlay all update together. The selectable set is the canonical
  // marquee/Select-All set (every selectable geometry element in the document).
  app_.setSelection(app_.selectableElements());
}

void EditorShell::copySelectedShapesToClipboard() {
  if (!app_.hasDocument() || !app_.hasSelection() || shapeClipboard_ == nullptr) {
    return;
  }
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(app_.document().document(), app_.selectedElements());
  if (!payload.has_value()) {
    return;
  }
  shapeClipboard_->setText(payload->toClipboardText());
}

void EditorShell::cutSelectedShapesToClipboard() {
  if (!app_.hasDocument() || !app_.hasSelection() || shapeClipboard_ == nullptr) {
    return;
  }
  // Copy first; only delete if the copy captured something serializable so a
  // failed copy never silently destroys the selection.
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(app_.document().document(), app_.selectedElements());
  if (!payload.has_value()) {
    return;
  }
  shapeClipboard_->setText(payload->toClipboardText());
  // `deleteSelectionWithUndo` records its own single source-level undo entry and
  // clears the selection, which is exactly the Cut contract (Copy + Delete as
  // one undoable step from the user's perspective).
  std::ignore = app_.deleteSelectionWithUndo(textEditor_.getText());
}

void EditorShell::pasteShapesFromClipboard(bool inFront) {
  if (!app_.hasDocument() || shapeClipboard_ == nullptr) {
    return;
  }
  const std::string clipboardText = shapeClipboard_->getText();
  std::optional<ShapeClipboardPayload> payload = ShapeClipboardPayload::parse(clipboardText);
  if (!payload.has_value()) {
    return;
  }

  svg::SVGDocument& document = app_.document().document();
  if (!document.hasSourceStore()) {
    return;
  }

  // A single selected group/svg is the preferred default-paste parent.
  std::optional<svg::SVGElement> selectedGroup;
  if (!inFront && app_.selectedElements().size() == 1u) {
    const svg::SVGElement& only = app_.selectedElements().front();
    const xml::XMLQualifiedNameRef tag = only.tagName();
    if (tag.name == "g" || tag.name == "svg") {
      selectedGroup = only;
    }
  }

  const PastePlacement placement =
      inFront ? PastePlacement::InFrontNoOffset : PastePlacement::EndOfRootOffset;
  PreparePasteResult prepared = preparePaste(document, *payload, placement, selectedGroup);
  if (!prepared.ok) {
    // Surface the user-visible error; the document is left untouched.
    std::cerr << prepared.error << '\n';
    return;
  }

  const std::string sourceBefore(document.source());

  // Build selection-restore targets from a scratch parse of the merged source.
  // The scratch tree is structurally identical to the post-reparse live tree,
  // so path-based writeback targets captured here resolve after the live
  // reparse — letting `restoreSelectionAfterNextDocumentReplace` select the
  // pasted elements without a live handle that does not yet exist.
  std::vector<AttributeWritebackTarget> selectionTargets;
  {
    ParseWarningSink sink;
    auto scratch = svg::parser::SVGParser::ParseSVG(prepared.mergedSource, sink);
    if (scratch.hasResult()) {
      svg::SVGDocument scratchDoc = scratch.result();
      for (const std::string& id : prepared.pastedElementIds) {
        if (auto element = scratchDoc.querySelector("#" + id); element.has_value()) {
          if (auto target = captureAttributeWritebackTarget(*element); target.has_value()) {
            selectionTargets.push_back(std::move(*target));
          }
        }
      }
    }
  }

  // Record one undo entry spanning the whole paste, mirroring the structural
  // source-edit undo used by delete / path operations.
  const svg::SVGElement undoAnchor = document.svgElement();
  app_.undoTimeline().record("Paste shapes",
                             captureDocumentSourceSnapshot(undoAnchor, sourceBefore),
                             captureDocumentSourceSnapshot(undoAnchor, prepared.mergedSource));

  app_.restoreSelectionAfterNextDocumentReplace(std::move(selectionTargets));
  app_.applyMutation(EditorCommand::PasteShapesCommand(std::move(prepared.mergedSource)));
}

bool EditorShell::selectionIsAllText() const {
  if (!app_.hasSelection()) {
    return false;
  }
  for (const svg::SVGElement& element : app_.selectedElements()) {
    if (element.tagName().name != svg::SVGTextElement::Tag) {
      return false;
    }
  }
  return true;
}

void EditorShell::convertSelectedTextToOutlines() {
  if (!app_.hasDocument() || !selectionIsAllText()) {
    return;
  }

  svg::SVGDocument& document = app_.document().document();
  if (!document.hasSourceStore()) {
    return;
  }
  const std::string sourceBefore(document.source());

  // Build every conversion first (detached DOM elements, no mutation) so any
  // failure abandons the command with the document untouched.
  struct PlannedConversion {
    svg::SVGElement text;
    svg::SVGElement parent;
    svg::SVGElement group;
    std::vector<svg::SVGElement> paths;
  };
  std::vector<PlannedConversion> planned;
  for (const svg::SVGElement& element : app_.selectedElements()) {
    const std::optional<svg::SVGElement> parent = element.parentElement();
    if (!parent.has_value()) {
      lastConvertTextError_ = "Convert to outlines failed: <text> element has no parent.";
      return;
    }
    ConvertTextToOutlinesResult result = convertTextToOutlines(document, element);
    if (!result.ok) {
      lastConvertTextError_ = result.error;
      return;
    }
    planned.push_back(PlannedConversion{
        .text = element,
        .parent = *parent,
        .group = *result.outlineGroup,
        .paths = std::move(result.outlinePaths),
    });
  }

  // Apply as ordinary structural DOM edits through the mutation seam: insert
  // each group before its <text> (preserving paint order), populate it, then
  // delete the <text>. Source reflection emits the deltas; entity identity
  // elsewhere in the document survives (no reparse).
  std::vector<svg::SVGElement> newSelection;
  newSelection.reserve(planned.size());
  for (PlannedConversion& plan : planned) {
    app_.applyMutation(EditorCommand::InsertElementCommand(plan.parent, plan.group, plan.text));
    for (svg::SVGElement& path : plan.paths) {
      app_.applyMutation(EditorCommand::InsertElementCommand(plan.group, path));
    }
    app_.applyMutation(EditorCommand::DeleteElementCommand(plan.text));
    newSelection.push_back(plan.group);
  }

  // Record one undo entry spanning the whole conversion once the queued edits
  // flush, mirroring the deferred source-edit undo used by the pen and text
  // authoring sessions.
  app_.recordDocumentSourceUndoOnNextFlush("Convert text to outlines", document.svgElement(),
                                           sourceBefore);
  app_.setSelection(std::move(newSelection));
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
      if (app_.setStylePropertyOnSelection(attrName, svgColor)) {
        flushQueuedMutationAndRefreshOverlay();
      } else {
        window_.wakeEventLoop();
      }
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
    Text,
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
      if (tool == ActiveTool::Select) {
        // Leaving the Pen tool commits any in-progress path as one undoable
        // command rather than dropping it.
        if (penTool_.commitOpenPath(app_)) {
          flushQueuedMutationAndRefreshOverlay();
        }
      }
      activeTool_ = tool;
    }
    if (icon == ToolButtonIcon::SelectPointer) {
      DrawSelectToolButtonIcon(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    } else if (icon == ToolButtonIcon::PenTool) {
      DrawPenToolButtonIcon(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    } else if (icon == ToolButtonIcon::Text) {
      const ImVec2 buttonMin = ImGui::GetItemRectMin();
      const ImVec2 buttonMax = ImGui::GetItemRectMax();
      const char* glyph = "T";
      const ImVec2 textSize = ImGui::CalcTextSize(glyph);
      drawList->AddText(ImVec2((buttonMin.x + buttonMax.x - textSize.x) * 0.5f,
                               (buttonMin.y + buttonMax.y - textSize.y) * 0.5f),
                        IM_COL32(230, 230, 230, 255), glyph);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    if (selected) {
      ImGui::PopStyleColor(3);
    }
  };

  const std::string selectTooltip = ToolTooltipText(ToolId::Select);
  const std::string penTooltip = ToolTooltipText(ToolId::Pen);
  const std::string textTooltip = ToolTooltipText(ToolId::Text);
  renderButton(ActiveTool::Select, "##select_tool", ToolButtonIcon::SelectPointer,
               selectTooltip.c_str());
  ImGui::SameLine(0.0f, kToolPaletteGap);
  renderButton(ActiveTool::Pen, "##pen_tool", ToolButtonIcon::PenTool, penTooltip.c_str());
  ImGui::SameLine(0.0f, kToolPaletteGap);
  renderButton(ActiveTool::Text, "##text_tool", ToolButtonIcon::Text, textTooltip.c_str());
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
    std::ignore = interactionController_.resetToActualSize();
    viewportInitialized_ = true;
  }

  // Capture chrome for the document version that is already being presented before this frame's
  // input can queue another geometry mutation. Ordinary geometry edits keep chrome on the
  // presented document version; active Pen drags can explicitly opt into live path chrome later in
  // the frame.
  if (!contentOnlyCaptureThisFrame_ && app_.hasDocument() &&
      app_.document().currentFrameVersion() ==
          renderCoordinator_.displayedDocVersionForDiagnostics()) {
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
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
  std::ignore = interactionController_.updatePanState(canvasHovered, spaceHeld, middleDown,
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
  const ScrollConsumptionResult scrollResult = interactionController_.consumeScrollEvents(
      inputBridge_.events(), paneRect, modalCapturingInput, kWheelZoomStep,
      kTrackpadPanPixelsPerScrollUnit);
  if (scrollResult.zoomChanged) {
    requestRenderAtEndOfFrame_ = true;
  }

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
  const bool textToolActive = activeTool_ == ActiveTool::Text;
  const auto cachedHandleIntentAt = [&](const Vector2d& documentPoint, bool includeRotate) {
    const auto& boundsCache = renderCoordinator_.selectionBoundsCache();
    if (boundsCache.lastSelection != app_.selectedElements()) {
      return SelectionTransformHandleIntent{};
    }
    const std::vector<Box2d>& boundsDoc = !boundsCache.displayedBoundsDoc.empty()
                                              ? boundsCache.displayedBoundsDoc
                                              : boundsCache.pendingBoundsDoc;
    return HitTestSelectionTransformHandles(boundsDoc, documentPoint,
                                            interactionController_.viewport().pixelsPerDocUnit(),
                                            includeRotate);
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
    hoverTransformIntent = cachedHandleIntentAt(screenToDocument(ImGui::GetMousePos()),
                                                /*includeRotate=*/!ImGui::GetIO().KeyShift);
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
  // Double-click while drafting commits the in-progress open path (no trailing
  // Z) as one undoable command, matching Enter. Checked before the click is
  // buffered so the double-click doesn't also place a stray anchor.
  if (penToolActive && toolEligible && penTool_.isDrafting() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    penTool_.commitOpenPath(app_);
    flushQueuedMutationAndRefreshOverlay();
  } else if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    preserveSourceEditFocusCursor_ = false;
    MouseModifiers modifiers;
    modifiers.shift = ImGui::GetIO().KeyShift;
    modifiers.option = ImGui::GetIO().KeyAlt;
    modifiers.command = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    modifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
    interactionController_.bufferPendingClick(screenToDocument(ImGui::GetMousePos()), modifiers);
    pendingSelectClickStartSeconds_ = ImGui::GetTime();
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
    const std::vector<Box2d>& redragBoundsDoc = !boundsCache.displayedBoundsDoc.empty()
                                                    ? boundsCache.displayedBoundsDoc
                                                    : boundsCache.pendingBoundsDoc;
    const std::vector<Box2d>& redragOccludingBoundsDoc =
        !boundsCache.displayedOccludingBoundsDoc.empty() ? boundsCache.displayedOccludingBoundsDoc
                                                         : boundsCache.pendingOccludingBoundsDoc;
    const SelectionTransformHandleIntent pendingHandleIntent =
        cacheMatchesSelection
            ? cachedHandleIntentAt(pendingClick.documentPoint,
                                   /*includeRotate=*/!pendingClick.modifiers.shift)
            : SelectionTransformHandleIntent{};
    bool tookFastRedrag = selectToolActive && cacheMatchesSelection &&
                          pendingHandleIntent.kind == SelectionTransformHandleKind::None &&
                          selectTool_.tryStartRedragOnSelected(
                              app_, pendingClick.documentPoint, pendingClick.modifiers,
                              redragBoundsDoc, redragOccludingBoundsDoc);
    if (!tookFastRedrag && renderCoordinator_.asyncRenderer().isBusy() && selectToolActive &&
        cacheMatchesSelection && pendingHandleIntent.kind == SelectionTransformHandleKind::None) {
      // The occlusion cache uses broad AABBs for later-painted elements. When the worker is busy,
      // prefer an optimistic re-drag of the current selection over freezing behind a conservative
      // false-positive overlap; the idle path above still uses full hit-testing for retargets.
      tookFastRedrag = selectTool_.tryStartRedragOnSelected(app_, pendingClick.documentPoint,
                                                            pendingClick.modifiers, redragBoundsDoc,
                                                            std::span<const Box2d>());
    }
    if (tookFastRedrag) {
      lastPostedScreenPoint_.reset();
      interactionController_.clearPendingClick();
      pendingClickFollowupAfterIdle_ = true;
    } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
      const bool leftMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
      const bool selectHoldElapsed =
          pendingSelectClickStartSeconds_.has_value() &&
          ImGui::GetTime() - *pendingSelectClickStartSeconds_ >= kSelectMarqueeHoldDelaySeconds;
      const bool selectDragIntent = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);
      const bool pendingClickHitsSelection =
          selectToolActive && pendingHandleIntent.kind == SelectionTransformHandleKind::None &&
          selectTool_.clickHitsCurrentSelection(app_, pendingClick.documentPoint);
      // A press that lands on an immediately-selectable element implicitly
      // selects-and-drags it; a marquee starts when the first click is on empty
      // space or on something that cannot be selected from the canvas, such as
      // a locked layer. Without this, a press-drag whose mouse-down was deferred
      // (worker busy) misclassifies as a marquee and the element is never
      // selected (gl_rnr GeodeDragZoomRerasterizes... selection-loss). We are in
      // the `!isBusy()` branch here, so `hitTest` is race-safe (same gate
      // `onMouseDown` uses).
      const bool pendingClickHitsImmediatelySelectableElement =
          selectToolActive && pendingHandleIntent.kind == SelectionTransformHandleKind::None &&
          !pendingClickHitsSelection &&
          selectTool_.clickHitsImmediatelySelectableElement(app_, pendingClick.documentPoint);
      const bool pendingClickCanStartMarquee =
          selectToolActive && pendingHandleIntent.kind == SelectionTransformHandleKind::None &&
          !pendingClickHitsSelection && !pendingClickHitsImmediatelySelectableElement;
      if (leftMouseDown && pendingClickCanStartMarquee && (selectHoldElapsed || selectDragIntent)) {
        lastPostedScreenPoint_.reset();
        selectTool_.beginMarquee(app_, pendingClick.documentPoint, pendingClick.modifiers.shift);
        renderCoordinator_.refreshSelectionBoundsCache(app_);
        requestRenderAtEndOfFrame_ = true;
        renderCoordinator_.rasterizeOverlayForCurrentSelection(
            app_, interactionController_.viewport(), selectTool_.marqueeRect(),
            selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
            selectionChromeDetailForActiveTool());
        interactionController_.clearPendingClick();
      } else if (leftMouseDown && pendingClickCanStartMarquee) {
        // Keep the click buffered until mouse-up selects, pointer movement starts marquee, or the
        // hold delay above starts marquee. This prevents full-canvas/background elements from being
        // selected just because the user is preparing a marquee drag.
      } else {
        // Slow path: full `onMouseDown` (hitTest + selection change +
        // possible drag start). Race-safe only when the worker is idle.
        lastPostedScreenPoint_.reset();
        bool queuedMutationForNextFrame = false;
        if (selectToolActive) {
          selectTool_.onMouseDown(app_, pendingClick.documentPoint, pendingClick.modifiers);
          if (!leftMouseDown) {
            selectTool_.onMouseUp(app_, pendingClick.documentPoint);
          }
        } else if (penToolActive) {
          penTool_.onMouseDown(app_, pendingClick.documentPoint, pendingClick.modifiers);
          if (!leftMouseDown && penTool_.isDraggingAnchor()) {
            penTool_.onMouseUp(app_, pendingClick.documentPoint);
          }
          queuedMutationForNextFrame = true;
        } else if (textToolActive) {
          // Text placement is a single click: insert the new `<text>` and
          // switch back to the Select tool so it can be moved and edited.
          textTool_.onMouseDown(app_, pendingClick.documentPoint, pendingClick.modifiers);
          activeTool_ = ActiveTool::Select;
          queuedMutationForNextFrame = true;
        }
        if (queuedMutationForNextFrame) {
          flushQueuedMutationAndRefreshOverlay();
        } else {
          renderCoordinator_.refreshSelectionBoundsCache(app_);
          requestRenderAtEndOfFrame_ = true;
          renderCoordinator_.rasterizeOverlayForCurrentSelection(
              app_, interactionController_.viewport(), selectTool_.marqueeRect(),
              selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
              selectionChromeDetailForActiveTool());
        }
        interactionController_.clearPendingClick();
      }
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
      // Local drag state is UI-thread owned and feeds composited presentation directly, so keep it
      // moving while the async renderer is busy. DOM writes are queued and coalesced until the
      // worker releases the document.
      if (ShouldPostDragMove<ImVec2>(currentScreen, lastPostedScreenPoint_,
                                     /*pendingFrameInFlight=*/false)) {
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
              app_, interactionController_.viewport(), selectTool_.marqueeRect(),
              selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
              selectionChromeDetailForActiveTool());
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
          const std::uint64_t settleTargetVersion = PostReleaseSettleTargetVersion(
              app_.document().currentFrameVersion(), app_.document().hasPendingMutations());
          renderCoordinator_.compositedPresentation().beginSettling(previewBeforeRelease,
                                                                    settleTargetVersion);
        }
        if (!renderCoordinator_.asyncRenderer().isBusy() &&
            (app_.flushFrame() || previewHadVisualChange)) {
          renderCoordinator_.compositedPresentation().beginSettling(
              previewBeforeRelease, app_.document().currentFrameVersion());
          renderCoordinator_.refreshSelectionBoundsCache(app_);
          renderCoordinator_.rasterizeOverlayForCurrentSelection(
              app_, interactionController_.viewport(), selectTool_.marqueeRect(), std::nullopt,
              std::nullopt, selectionChromeDetailForActiveTool());
        }
      } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
        renderCoordinator_.refreshSelectionBoundsCache(app_);
        renderCoordinator_.rasterizeOverlayForCurrentSelection(
            app_, interactionController_.viewport(), selectTool_.marqueeRect(), std::nullopt,
            std::nullopt, selectionChromeDetailForActiveTool());
      }
    }
  }

  if (!interactionController_.pendingClick().has_value()) {
    pendingSelectClickStartSeconds_.reset();
  }

  if (penToolActive && penTool_.isDraggingAnchor()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
      MouseModifiers dragModifiers;
      dragModifiers.shift = ImGui::GetIO().KeyShift;
      dragModifiers.option = ImGui::GetIO().KeyAlt;
      dragModifiers.command = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
      dragModifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
      penTool_.onMouseMove(app_, screenToDocument(ImGui::GetMousePos()), /*buttonHeld=*/true,
                           dragModifiers);
      if (app_.document().hasPendingMutations() && flushQueuedMutationAndRefreshOverlay()) {
        penDragFlushedThisFrame_ = true;
      }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      penTool_.onMouseUp(app_, screenToDocument(ImGui::GetMousePos()));
      flushQueuedMutationAndRefreshOverlay();
    }
  }

  // Pen hover chrome: while drafting with the button up, rubber-band the
  // segment a click would commit and highlight the close-path target when the
  // pointer is within closing range.
  if (penToolActive && penTool_.isDrafting() && !penTool_.isDraggingAnchor() && !spaceHeld &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    MouseModifiers hoverModifiers;
    hoverModifiers.shift = ImGui::GetIO().KeyShift;
    hoverModifiers.option = ImGui::GetIO().KeyAlt;
    hoverModifiers.command = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    hoverModifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
    const Vector2d hoverDocPoint = screenToDocument(ImGui::GetMousePos());
    renderCoordinator_.setPenHoverChrome(penTool_.previewSegmentPath(hoverDocPoint, hoverModifiers),
                                         penTool_.wouldCloseAt(hoverDocPoint, hoverModifiers)
                                             ? std::make_optional(penTool_.draftStartPoint())
                                             : std::nullopt);
  } else {
    renderCoordinator_.setPenHoverChrome(std::nullopt, std::nullopt);
  }

  applyPendingDocumentSpaceReplayInputForTesting();

  if (!renderCoordinator_.asyncRenderer().isBusy() && app_.hasDocument()) {
    requestRenderAtEndOfFrame_ = true;
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
  const Entity suppressedLayerEntity = renderCoordinator_.suppressedCompositedLayerEntity(app_);
  const bool suppressDragTargetTiles = renderCoordinator_.selectedElementIsDisplayNone(app_);
  const bool hasPresentableActiveDragTarget = HasPresentableDragTargetTile(
      textures_, activeDragPreview, suppressedLayerEntity, suppressDragTargetTiles);
  const auto representedDragPreview = OverlayRepresentedDragPreviewForPresentation(
      activeDragPreview, displayedDragPreview, hasPresentableActiveDragTarget);
  const auto representedGesturePreview = OverlayGesturePreviewForPresentation(
      activeGesturePreview, liveActiveDragPreview, representedDragPreview);
  if (!contentOnlyCaptureThisFrame_) {
    // While the Pen tool is active the selected path is itself the live
    // interaction surface, so chrome must track the live DOM even between
    // anchor drags (close-path clicks, deferred clicks processed after
    // mouse-release).
    const bool allowLivePenOverlay = penToolActive;
    updatePenLivePreviewTarget();
    renderCoordinator_.rasterizeOverlayForPresentation(
        app_, selectTool_, interactionController_.viewport(), textures_, activeDragPreview,
        representedDragPreview, selectionChromeDetailForActiveTool(), allowLivePenOverlay);
  }
  // While the pen live preview is active, the overlay snapshot itself presents
  // the edited path's document pixels (captured from the same post-flush DOM
  // as the chrome). Suppress the path's stale composited layer tile so the
  // previous geometry doesn't show through underneath the preview.
  // Content-only captures carry no chrome — and therefore no preview — so
  // they keep presenting the raster tile.
  Entity penPreviewSuppressedEntity = entt::null;
  if (!contentOnlyCaptureThisFrame_ && renderCoordinator_.immediateOverlaySnapshot().has_value() &&
      renderCoordinator_.immediateOverlaySnapshot()->livePathPreview.has_value()) {
    penPreviewSuppressedEntity =
        renderCoordinator_.immediateOverlaySnapshot()->livePathPreview->entity;
  }
  const Entity presentSuppressedLayerEntity =
      penPreviewSuppressedEntity != entt::null ? penPreviewSuppressedEntity : suppressedLayerEntity;
  interactionController_.frameHistory().setLatestMemorySample(
      MemorySampleFromPresentationResources(textures_.presentationResourceStats()));
  // Selection chrome is rendered exclusively by Donner's OverlayRenderer drawn
  // straight onto the Geode framebuffer via this direct-render callback. There
  // is no ImGui-vector or texture-blit fallback: edge frames that can't take
  // this path (a content-only capture, which intentionally carries no chrome,
  // or a viewport with no presentable clip rect, which has nowhere to draw)
  // simply skip the overlay — clearing the callback leaves the framebuffer
  // chrome-free for that frame.
  bool documentPresentedDirectly = false;
#ifdef DONNER_EDITOR_WGPU
  window_.setWgpuUnderlayRenderCallback({});
  const std::optional<Box2d> directDocumentClipRect =
      PresentedImageClipRect(paneRect, interactionController_.viewport().imageScreenRect());
  const auto isDirectlyPresentableTile = [&](const GlTextureCache::TileView& tile) {
    if (!ShouldPresentCompositedTile(tile, presentSuppressedLayerEntity, suppressDragTargetTiles) ||
        (suppressDragTargetTiles && TileMatchesActiveDragPreview(tile, activeDragPreview))) {
      return false;
    }
    return tile.textureSnapshot != nullptr &&
           tile.textureSnapshot->backend() == svg::RendererTextureSnapshotBackend::Geode;
  };
  const auto canPresentTileSetDirectly = [&](const std::vector<GlTextureCache::TileView>& tiles) {
    return std::ranges::all_of(tiles, [&](const GlTextureCache::TileView& tile) {
      if (!ShouldPresentCompositedTile(tile, presentSuppressedLayerEntity,
                                       suppressDragTargetTiles) ||
          (suppressDragTargetTiles && TileMatchesActiveDragPreview(tile, activeDragPreview))) {
        return true;
      }
      return tile.textureSnapshot != nullptr &&
             tile.textureSnapshot->backend() == svg::RendererTextureSnapshotBackend::Geode;
    });
  };
  const auto hasDirectlyPresentableTile = [&](const std::vector<GlTextureCache::TileView>& tiles) {
    return std::ranges::any_of(tiles, isDirectlyPresentableTile);
  };
  const bool drawOverviewTiles =
      ShouldPresentOverviewTiles(textures_.activeTilesViewportBounded(), textures_.overviewTiles());
  if (!contentOnlyCaptureThisFrame_ && directDocumentRenderer_ != nullptr &&
      directDocumentClipRect.has_value() &&
      ((drawOverviewTiles && hasDirectlyPresentableTile(textures_.overviewTiles())) ||
       hasDirectlyPresentableTile(textures_.tiles())) &&
      (!drawOverviewTiles || canPresentTileSetDirectly(textures_.overviewTiles())) &&
      canPresentTileSetDirectly(textures_.tiles())) {
    documentPresentedDirectly = true;
    const ViewportState documentViewport = interactionController_.viewport();
    const Box2d documentClipRect = *directDocumentClipRect;
    std::vector<GlTextureCache::TileView> directOverviewTiles;
    if (drawOverviewTiles) {
      directOverviewTiles.assign(textures_.overviewTiles().begin(),
                                 textures_.overviewTiles().end());
    }
    std::vector<GlTextureCache::TileView> directTiles(textures_.tiles().begin(),
                                                      textures_.tiles().end());
    window_.setWgpuUnderlayRenderCallback(
        [this, documentViewport, documentClipRect,
         directOverviewTiles = std::move(directOverviewTiles), directTiles = std::move(directTiles),
         activeDragPreview, displayedDragPreview, presentSuppressedLayerEntity,
         suppressDragTargetTiles](const gui::EditorWindowWgpuRenderTarget& target) {
          if (directCheckerboardRenderer_ == nullptr || directDocumentRenderer_ == nullptr) {
            return;
          }
          lastDirectPresentationCost_ = DrawDocumentPresentationToFramebuffer(
              *directCheckerboardRenderer_, *directDocumentRenderer_, target, documentViewport,
              documentClipRect, directOverviewTiles, directTiles, activeDragPreview,
              displayedDragPreview, presentSuppressedLayerEntity, suppressDragTargetTiles);
        });
  }

  window_.setWgpuDirectRenderCallback({});
  const std::optional<Box2d> directOverlayClipRect =
      PresentedImageClipRect(paneRect, interactionController_.viewport().imageScreenRect());
  if (!contentOnlyCaptureThisFrame_ && directOverlayRenderer_ != nullptr &&
      renderCoordinator_.immediateOverlaySnapshot().has_value() &&
      directOverlayClipRect.has_value()) {
    SelectionChromeSnapshot overlaySnapshot = *renderCoordinator_.immediateOverlaySnapshot();
    ViewportState overlayViewport = interactionController_.viewport();
    const Box2d overlayClipRect = *directOverlayClipRect;
    window_.setWgpuDirectRenderCallback(
        [this, overlaySnapshot = std::move(overlaySnapshot), overlayViewport,
         overlayClipRect](const gui::EditorWindowWgpuRenderTarget& target) {
          if (directOverlayRenderer_ == nullptr) {
            return;
          }
          DrawImmediateOverlaySnapshotToFramebuffer(
              *directOverlayRenderer_, target, overlayViewport, overlayClipRect, overlaySnapshot);
        });
  }
#endif
  RenderPanePresenterState paneState{
      .viewport = interactionController_.viewport(),
      .frameHistory = interactionController_.frameHistory(),
      .textures = textures_,
      .immediateOverlaySnapshot = renderCoordinator_.immediateOverlaySnapshot(),
      .activeDragPreview = activeDragPreview,
      .displayedDragPreview = displayedDragPreview,
      .contentRegion = Vector2d(contentRegion.x, contentRegion.y),
      .suppressedLayerEntity = presentSuppressedLayerEntity,
      .suppressDragTargetTiles = suppressDragTargetTiles,
      .documentPresentedDirectly = documentPresentedDirectly,
      .showFrameGraph = showPerfOverlay_ && !contentOnlyCaptureThisFrame_,
  };
  renderPanePresenter_.render(paneState);
  if (!contentOnlyCaptureThisFrame_) {
    renderSelectionSizeChip(hoverTransformIntent, representedGesturePreview);
    renderReferenceHighlightChip();
    renderToolPalette(paneOriginImGui, contentRegion);
    renderRenderPaneContextMenu();
  }

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
    layersPanel_.refreshSnapshot(app_, &layerThumbnailRenderer_);
  }
  EditorApp* liveAppForClicks = rendererBusy ? nullptr : &app_;

  // The user-facing Layers panel replaces the old XML tree view in this
  // sidebar window. `SidebarPresenter` is still the
  // Inspector backend below (`renderInspector`), and its `renderTreeView` /
  // tests remain; it is simply no longer the user-facing outline.
  ImGui::SetNextWindowPos(ImVec2(rightPaneX, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.treePaneHeight), ImGuiCond_Always);
  ImGui::Begin("Layers", nullptr, paneFlags);
  std::vector<std::uint64_t> liveThumbnailKeys;
  liveThumbnailKeys.reserve(layersPanel_.rows().size() + 4u);
  // Donner renders each row's preview to a bitmap during refreshSnapshot; here
  // we only upload that bitmap to a GL/WGPU texture and hand the handle back so
  // ImGui can blit it via ImGui::Image. ImGui never draws the vector artwork.
  const LayersPanel::ThumbnailTextureProvider thumbnailTextureProvider =
      [this](std::uint64_t stableId,
             const svg::RendererBitmap& bitmap) -> LayersPanel::ThumbnailTexture {
    const GlTextureCache::ThumbnailTextureView uploaded =
        thumbnailTextures_.uploadThumbnail(stableId, bitmap);
    return LayersPanel::ThumbnailTexture{
        .texture = uploaded.texture,
        .uvBottomRight = uploaded.uvBottomRight,
    };
  };
  const LayersPanel::IconTextureProvider layerIconTextureProvider =
      [this, &liveThumbnailKeys](std::uint64_t stableId,
                                 const svg::RendererBitmap& bitmap) -> LayersPanel::IconTexture {
    liveThumbnailKeys.push_back(stableId);
    const GlTextureCache::ThumbnailTextureView uploaded =
        thumbnailTextures_.uploadThumbnail(stableId, bitmap);
    return LayersPanel::IconTexture{
        .texture = uploaded.texture,
        .uvBottomRight = uploaded.uvBottomRight,
    };
  };
  const SidebarPresenter::IconTextureProvider sidebarIconTextureProvider =
      [this, &liveThumbnailKeys](
          std::uint64_t stableId,
          const svg::RendererBitmap& bitmap) -> SidebarPresenter::IconTexture {
    liveThumbnailKeys.push_back(stableId);
    const GlTextureCache::ThumbnailTextureView uploaded =
        thumbnailTextures_.uploadThumbnail(stableId, bitmap);
    return SidebarPresenter::IconTexture{
        .texture = uploaded.texture,
        .uvBottomRight = uploaded.uvBottomRight,
    };
  };
  // Feed the current locked-rejection flash into the Layers panel so the rejected
  // (locked) element's row flashes red in sync with the canvas outline flash. The
  // flash was already ticked earlier this frame (see runFrame's
  // tickLockedRejectionFlash), and the shell keeps the event loop awake while it
  // fades, so the row fade animates for free. `std::nullopt` clears it.
  if (const std::optional<SelectTool::LockedRejectionFlash> flash =
          selectTool_.lockedRejectionFlash();
      flash.has_value()) {
    layersPanel_.setLockedRejectionFlash(
        LayersLockedRejectionFlash{.element = flash->element, .intensity = flash->intensity});
  } else {
    layersPanel_.setLockedRejectionFlash(std::nullopt);
  }
  layersPanel_.render(liveAppForClicks, thumbnailTextureProvider, layerIconTextureProvider);
  // Feed the Layers-panel hover into the shared source-hover preview so the
  // canvas (and source pane) highlight the hovered element, the same way
  // hovering a source-pane token does.
  layersPanelHoverElement_ = layersPanel_.hoveredElement();
  updateSourceHoverPreview();
  if (layersPanel_.consumeSelectionChanged()) {
    // Reuse the existing tree-origin selection-sync plumbing so a Layers-row
    // selection change is reflected in the canvas and source panes the same way
    // the old tree view's selection was.
    preserveSourceEditFocusCursor_ = false;
    treeSelectionOriginatedInTree_ = true;
    treeviewPendingScroll_ = false;
  }
  // A Layers-panel show/hide, lock, rename, reorder, or z-order click queues a
  // DOM mutation. Flush it and refresh the overlay the same way the Inspector
  // panels do (above) — otherwise the queued mutation never reaches the worker
  // and the canvas keeps presenting the pre-mutation frame (the hidden-layer
  // "ghost").
  if (layersPanel_.consumeQueuedMutation()) {
    flushQueuedMutationAndRefreshOverlay();
  }
  ImGui::End();
  lastTreeSelection_ = app_.selectedElement();

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.inspectorPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.inspectorPaneHeight), ImGuiCond_Always);
  ImGui::Begin("Inspector", nullptr, paneFlags);
  const bool inspectorQueuedMutation = sidebarPresenter_.renderInspector(
      liveAppForClicks, interactionController_.viewport(), sidebarIconTextureProvider);
  // Text-property inspector: shown only when the selection is exactly one
  // `<text>` element. Content/style edits route through the mutation seam.
  const bool textInspectorQueuedMutation =
      textInspectorPanel_.render(liveAppForClicks, ImGui::GetTime());
  ImGui::End();
  if (inspectorQueuedMutation || textInspectorQueuedMutation) {
    flushQueuedMutationAndRefreshOverlay();
  }
  // Release Layers/Inspector preview textures whose rows/icons are no longer
  // visible so the GPU texture set does not grow across refreshes.
  for (const LayerTreeRow& row : layersPanel_.rows()) {
    liveThumbnailKeys.push_back(row.stableId);
  }
  thumbnailTextures_.retainThumbnailsOnly(liveThumbnailKeys);

  // The Compositor Debug panel is a developer diagnostics view, hidden unless
  // toggled on from the View menu. The user-facing Layers/Inspector panes above
  // are unaffected.
  if (!showCompositorDebugPanel_ || layerPanelDetached_) {
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.layerPanelPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.layerPanelHeight), ImGuiCond_Always);
  ImGui::Begin("Compositor Debug##docked_compositor_debug", nullptr, paneFlags);
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
  const auto renderStats = renderCoordinator_.asyncRenderer().compositorRenderFrameStats();
  const PresentationCoverageDiagnostics coverageDiagnostics = textures_.coverageDiagnostics();
  compositorDebugPanel_.render(compositeTiles, compositorState, workerCompositorEntity,
                               viewport.zoom, viewport.devicePixelRatio, viewportDesiredCanvas,
                               documentCanvas, coverageDiagnostics, fastPath, renderStats);
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
                    textColor, "Compositor Debug");

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
  if (!showCompositorDebugPanel_ || !layerPanelDetached_) {
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
  ImGui::Begin("Compositor Debug##floating_compositor_debug", &layerPanelOpen, kFloatingFlags);
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
  if (layersPanelHoverElement_.has_value()) {
    const std::array<svg::SVGElement, 1> hovered{*layersPanelHoverElement_};
    AddUniqueElements(&elements, hovered);
  }
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
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
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

SelectionChromeDetail EditorShell::selectionChromeDetailForActiveTool() const {
  if (activeTool_ == ActiveTool::Pen) {
    return SelectionChromeDetail::PathOutlinesOnly;
  }
  return SelectionChromeDetail::Full;
}

void EditorShell::updatePenLivePreviewTarget() {
  std::optional<svg::SVGElement> target;
  if (app_.hasDocument() && activeTool_ == ActiveTool::Pen &&
      app_.selectedElements().size() == 1u) {
    const svg::SVGElement& selected = app_.selectedElements().front();
    if (penTool_.isDrafting()) {
      target = selected;
    } else if (renderCoordinator_.penLivePreviewElement() ==
                   std::optional<svg::SVGElement>(selected) &&
               app_.document().currentFrameVersion() >
                   renderCoordinator_.displayedDocVersionForDiagnostics()) {
      // The pen session just ended (close-path, commit, or point-edit
      // release). Keep the live preview + tile suppression until the async
      // raster of the final geometry lands, so the committed path never snaps
      // back to its pre-commit pixels for a frame.
      target = selected;
    }
  }
  renderCoordinator_.setPenLivePreviewElement(std::move(target));
}

bool EditorShell::flushQueuedMutationAndRefreshOverlay() {
  if (renderCoordinator_.asyncRenderer().isBusy()) {
    window_.wakeEventLoop();
    return false;
  }

  if (!app_.flushFrame()) {
    window_.wakeEventLoop();
    return false;
  }

  renderCoordinator_.invalidatePresentationAfterDocumentFlush(app_,
                                                              app_.document().lastFlushResult());
  documentSyncController_.applyPendingWritebacks(app_, selectTool_, textEditor_);
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  updatePenLivePreviewTarget();
  // Any pen-tool flush is live geometry the chrome must track immediately —
  // not just active anchor drags. Close-path clicks and deferred clicks
  // processed after mouse-release flush without isDraggingAnchor(), and gating
  // them on the displayed doc version left the overlay one gesture behind
  // until the async render landed.
  const bool allowLivePenOverlay = activeTool_ == ActiveTool::Pen;
  if (allowLivePenOverlay || app_.document().currentFrameVersion() <=
                                 renderCoordinator_.displayedDocVersionForDiagnostics()) {
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
  }
  requestRenderAtEndOfFrame_ = true;
  window_.wakeEventLoop();
  return true;
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

  // Arrange (paint/z-order). Acts on the right-clicked element, or on the single
  // selection when the menu was opened over empty canvas. Reuses the shared
  // DOM-level reorderSelectedElement engine — the same path as the Cmd+[ / Cmd+]
  // shortcuts — so the move is undoable and reflected back into the source.
  std::optional<svg::SVGElement> arrangeTarget;
  if (renderContextMenuHitElement_.has_value()) {
    arrangeTarget = *renderContextMenuHitElement_;
  } else if (app_.selectedElements().size() == 1u) {
    arrangeTarget = app_.selectedElements().front();
  }
  if (ImGui::BeginMenu("Arrange", arrangeTarget.has_value())) {
    const auto arrange = [&](EditorApp::ZOrder direction) {
      app_.setSelection(*arrangeTarget);
      selectionChanged = true;
      std::ignore = app_.reorderSelectedElement(direction);
    };
    if (ImGui::MenuItem("Bring to Front", "Cmd+Shift+]")) {
      arrange(EditorApp::ZOrder::BringToFront);
    }
    if (ImGui::MenuItem("Bring Forward", "Cmd+]")) {
      arrange(EditorApp::ZOrder::BringForward);
    }
    if (ImGui::MenuItem("Send Backward", "Cmd+[")) {
      arrange(EditorApp::ZOrder::SendBackward);
    }
    if (ImGui::MenuItem("Send to Back", "Cmd+Shift+[")) {
      arrange(EditorApp::ZOrder::SendToBack);
    }
    ImGui::EndMenu();
  }

  std::optional<svg::SVGElement> unbundleTarget;
  if (renderContextMenuHitElement_.has_value()) {
    unbundleTarget = *renderContextMenuHitElement_;
  }
  const PathOperationAvailability unbundleAvailability =
      app_.compoundPathUnbundleAvailability(unbundleTarget);
  if (ImGui::MenuItem("Unbundle Compound Path", nullptr, false, unbundleAvailability.canApply)) {
    selectionChanged = app_.unbundleCompoundPath(unbundleTarget);
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
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
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
        .chipKind = contribution.kind == StyleContributionKind::ReferenceResourceElement
                        ? TextEditor::SourceStyleChipKind::ReferenceCount
                        : TextEditor::SourceStyleChipKind::SelectorMatchCount,
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
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
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
          app_, interactionController_.viewport(), selectTool_.marqueeRect(),
          selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
          selectionChromeDetailForActiveTool());
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
  ++frameTelemetryFrame_;
  renderCoordinator_.beginFrameCostTracking();
  FrameCostBreakdown::MainFrame mainFrameCost;
  const FrameCostBreakdown::DirectPresentation directPresentationCost = lastDirectPresentationCost_;
  lastDirectPresentationCost_ = FrameCostBreakdown::DirectPresentation{};
  auto phaseStart = std::chrono::steady_clock::now();
  const auto markPhase = [&](double& destination) {
    const auto now = std::chrono::steady_clock::now();
    destination += std::chrono::duration<double, std::milli>(now - phaseStart).count();
    phaseStart = now;
  };
  contentOnlyCaptureThisFrame_ = contentOnlyCaptureForNextFrame_;
  contentOnlyCaptureForNextFrame_ = false;
  textures_.advancePresentationFrame();
  compositorDebugPanel_.advancePresentationFrame();
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
  const float frameDeltaMs = ImGui::GetIO().DeltaTime * 1000.0f;
  interactionController_.noteFrameDelta(frameDeltaMs);

  // Advance the locked-rejection flash (clicking a locked element flashes its outline red) and push
  // the current flash state to the render coordinator so the next overlay capture draws it. Capture
  // whether a flash was active *before* the tick: the overlay rasterize below (after the frame
  // flush) must also run on the final intensity→0 frame to erase the outline, and while the flash
  // animates the editor is event-driven so the loop must keep waking.
  const bool hadLockedRejectionFlash = renderCoordinator_.hasLockedRejectionFlash();
  selectTool_.tickLockedRejectionFlash(ImGui::GetIO().DeltaTime);
  const std::optional<SelectTool::LockedRejectionFlash> lockedRejectionFlash =
      selectTool_.lockedRejectionFlash();
  renderCoordinator_.setLockedRejectionFlash(lockedRejectionFlash);
  const bool lockedRejectionFlashNeedsRedraw =
      lockedRejectionFlash.has_value() || hadLockedRejectionFlash;
  if (lockedRejectionFlash.has_value()) {
    window_.wakeEventLoop();
  }

  updateWindowTitle();
  markPhase(mainFrameCost.preparationMs);

  renderCoordinator_.pollRenderResult(app_, interactionController_.viewport(), textures_,
                                      &interactionController_.frameHistory());
  markPhase(mainFrameCost.renderPollMs);

  if (!renderCoordinator_.asyncRenderer().isBusy()) {
    if (app_.flushFrame()) {
      renderCoordinator_.invalidatePresentationAfterDocumentFlush(
          app_, app_.document().lastFlushResult());
      renderCoordinator_.refreshSelectionBoundsCache(app_);
      // UI mutations can be queued while the async renderer is busy. When they flush here, the
      // DOM/source panes already reflect the edit, so request a matching document render instead of
      // continuing to present stale composited textures.
      requestRenderAtEndOfFrame_ = true;
    }
  }
  markPhase(mainFrameCost.documentFlushMs);

  // Re-rasterize the overlay against the just-flushed DOM while a locked-rejection flash is (or was
  // just) active. A locked click never changes selection, so none of the selection-/hover-driven
  // overlay rasterize triggers fire for the flash — this is what animates the fade and erases the
  // outline on the final frame.
  if (lockedRejectionFlashNeedsRedraw && app_.hasDocument() &&
      !renderCoordinator_.asyncRenderer().isBusy()) {
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), selectTool_.marqueeRect(),
        selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview(),
        selectionChromeDetailForActiveTool());
  }
  markPhase(mainFrameCost.overlayRefreshMs);

  documentSyncController_.syncParseErrorMarkers(app_, textEditor_);
  documentSyncController_.applyPendingWritebacks(app_, selectTool_, textEditor_);
  markPhase(mainFrameCost.documentSyncMs);

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
  markPhase(mainFrameCost.layoutMs);

  handleGlobalShortcuts();
  markPhase(mainFrameCost.shortcutsMs);

  MenuBarState menuState{
      .sourcePaneFocused = sourcePaneVisible_ && textEditor_.isFocused(),
      .canSave = app_.hasDocument(),
      .canRevert = app_.hasDocument() && app_.isDirty() && !app_.cleanSourceText().empty(),
      .canUndo = app_.canUndo(),
      .canRedo = app_.canRedo(),
      .sourceFocusMode = sourceFocusMode_,
      .hasShapeSelection = app_.hasSelection(),
      .hasShapeClipboard = shapeClipboard_ != nullptr && shapeClipboard_->hasText(),
      .hasTextSelection = selectionIsAllText(),
      .hasSelectableElements = canvasHasSelectableElements(),
      .showCompositorDebugPanel = showCompositorDebugPanel_,
      .showPerfOverlay = showPerfOverlay_,
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
  if (menuActions.exportViewportSvg) {
    requestExportViewportSvg(/*includeOverlay=*/false);
  }
  if (menuActions.exportViewportSvgWithOverlay) {
    requestExportViewportSvg(/*includeOverlay=*/true);
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
  const bool sourcePaneFocusedForMenu = sourcePaneVisible_ && textEditor_.isFocused();
  if (menuActions.cut) {
    if (sourcePaneFocusedForMenu) {
      textEditor_.cut();
    } else {
      cutSelectedShapesToClipboard();
    }
  }
  if (menuActions.copy) {
    if (sourcePaneFocusedForMenu) {
      textEditor_.copy();
    } else {
      copySelectedShapesToClipboard();
    }
  }
  if (menuActions.paste) {
    if (sourcePaneFocusedForMenu) {
      textEditor_.paste();
    } else {
      pasteShapesFromClipboard(/*inFront=*/false);
    }
  }
  if (menuActions.pasteInFront) {
    pasteShapesFromClipboard(/*inFront=*/true);
  }
  if (menuActions.convertTextToOutlines) {
    convertSelectedTextToOutlines();
  }
  if (menuActions.selectAll) {
    textEditor_.selectAll();
  }
  if (menuActions.selectAllCanvas) {
    // Same canonical canvas Select-All path as the Cmd+A shortcut.
    selectAllCanvasElements();
  }
  if (menuActions.deselectAll) {
    // Source/XML pane focused: collapse the text selection to the caret, mirroring the
    // focus-aware Cmd+Shift+A shortcut.
    textEditor_.clearSelection();
  }
  if (menuActions.deselectAllCanvas) {
    // Same canonical clear path as the Cmd+Shift+A shortcut and Escape.
    app_.clearSelection();
  }
  if (menuActions.zoomIn) {
    if (interactionController_.applyZoom(kKeyboardZoomStep,
                                         interactionController_.viewport().paneCenter())) {
      requestRenderAtEndOfFrame_ = true;
    }
  }
  if (menuActions.zoomOut) {
    if (interactionController_.applyZoom(1.0 / kKeyboardZoomStep,
                                         interactionController_.viewport().paneCenter())) {
      requestRenderAtEndOfFrame_ = true;
    }
  }
  if (menuActions.actualSize) {
    if (interactionController_.resetToActualSize()) {
      requestRenderAtEndOfFrame_ = true;
    }
  }
  if (menuActions.toggleSourceFocusMode) {
    toggleSourceFocusMode();
  }
  const bool showCompositorDebugPanelBeforeMenu = showCompositorDebugPanel_;
  const bool showPerfOverlayBeforeMenu = showPerfOverlay_;
  ApplyViewMenuToggleActions(menuActions, &showCompositorDebugPanel_, &showPerfOverlay_);
  if (showCompositorDebugPanel_ != showCompositorDebugPanelBeforeMenu ||
      showPerfOverlay_ != showPerfOverlayBeforeMenu) {
    window_.wakeEventLoop();
  }

  dialogPresenter_.render(
      [this](std::string_view path, std::string* error) { return tryOpenPath(path, error); },
      [this](std::string_view path, std::string* error) {
        return pendingViewportExport_ ? tryExportViewportSvgToPath(path, error)
                                      : trySavePath(path, error);
      });
  markPhase(mainFrameCost.menusDialogsMs);

  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  std::ignore = highlightSelectionSourceIfNeeded();
  if (sourcePaneVisible_) {
    renderSourcePane(paneOriginY, paneHeight, mainPaneLayout.sourcePaneWidth, codeFont_);
  }
  markPhase(mainFrameCost.sourcePaneMs);
  renderRenderPane(renderPaneOrigin, renderPaneSize, kPaneFlags | ImGuiWindowFlags_NoBackground);
  markPhase(mainFrameCost.renderPaneMs);
  renderSidebars(rightPaneX, rightPaneWidth_, paneOriginY, rightSidebarLayout, kPaneFlags);
  if (highlightSelectionSourceIfNeeded()) {
    window_.wakeEventLoop();
  }
  markPhase(mainFrameCost.sidebarsMs);
  renderSourcePaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight,
                           mainPaneLayout.sourcePaneWidth);
  renderRightPaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight);
  if (!layerPanelDetached_) {
    renderLayerPanelSplitter(rightPaneX, rightPaneWidth_, rightSidebarLayout);
  }
  renderFloatingLayerPanel();
  markPhase(mainFrameCost.splittersMs);
  if (requestRenderAtEndOfFrame_) {
    if (!app_.hasDocument()) {
      requestRenderAtEndOfFrame_ = false;
    } else if (penDragFlushedThisFrame_) {
      // An actively-moving pen drag flushed new path geometry this frame. The
      // live preview already presents that geometry, so skip the async render
      // request: keeping the worker idle guarantees the next drag frame's
      // flush is never blocked behind a busy render (the source of pen-drag
      // frame stutter). The request flag stays set and fires on the first
      // frame the pointer pauses; wake the loop so that frame happens even
      // without further input.
      window_.wakeEventLoop();
    } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
      renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                            &textures_);
      requestRenderAtEndOfFrame_ = false;
    } else {
      window_.wakeEventLoop();
    }
  }
  penDragFlushedThisFrame_ = false;
  markPhase(mainFrameCost.endRenderRequestMs);
  FrameCostBreakdown frameCost = renderCoordinator_.lastFrameCostBreakdown();
  const gui::EditorWindowFrameTiming& hostEndFrameTiming = window_.lastEndFrameTiming();
  frameCost.mainFrame = mainFrameCost;
  frameCost.hostFrame = FrameCostBreakdown::HostFrame{
      .beginFrameMs = window_.lastBeginFrameMs(),
      .previousEndFrameMs = hostEndFrameTiming.endFrameMs,
      .previousImguiRenderMs = hostEndFrameTiming.imguiRenderMs,
      .previousSurfaceAcquireMs = hostEndFrameTiming.surfaceAcquireMs,
      .previousUnderlayMs = hostEndFrameTiming.underlayMs,
      .previousImguiDrawMs = hostEndFrameTiming.imguiDrawMs,
      .previousDirectMs = hostEndFrameTiming.directMs,
      .previousReadbackMs = hostEndFrameTiming.readbackMs,
      .previousPresentMs = hostEndFrameTiming.presentMs,
  };
  frameCost.directPresentation = directPresentationCost;
  if (sourcePaneVisible_) {
    frameCost.sourceRopes = textEditor_.lastSourceRopeCost();
  }
  latestFrameCostForReadback_ = frameCost;
  interactionController_.frameHistory().setLatestFrameCost(frameCost);
  const PresentationResourceStats presentationResources = textures_.presentationResourceStats();
  interactionController_.frameHistory().setLatestMemorySample(
      MemorySampleFromPresentationResources(presentationResources));
  maybeLogFrameMissTelemetry(frameCost);
  maybeLogResourceDiagnostics(frameCost);
  contentOnlyCaptureThisFrame_ = false;
}

}  // namespace donner::editor
