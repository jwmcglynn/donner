#pragma once
/// @file

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/FocusView.h"
#include "donner/editor/FrameMissTelemetry.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/SourceSelection.h"
#include "donner/editor/ViewportInteractionController.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/properties/PaintServer.h"

namespace donner::editor {
struct EditorShellOptions;
}

namespace donner::editor::internal {

/// Source range for a referenced paint server shown by the toolbar.
struct ToolbarPaintReferenceState {
  std::string href;
  bool external = false;
  std::optional<SourceByteRange> sourceRange;
};

/// Paint slot presentation state for the fill/stroke toolbar.
struct ToolbarPaintSlotState {
  css::RGBA color = css::RGBA::RGB(0, 0, 0);
  bool isNone = true;
  bool isCustom = false;
  std::optional<ToolbarPaintReferenceState> reference;
  std::string customLabel;
};

/// Fill and stroke toolbar paint state.
struct ToolbarPaintState {
  ToolbarPaintSlotState fill;
  ToolbarPaintSlotState stroke;
};

/// High-level action for a buffered canvas click before the idle-only hit-test path.
enum class PendingClickBusyAction {
  RunIdleClickPath,
  CompleteFastRedrag,
  CancelBusyRender,
};

/// High-level action for a buffered click once the renderer is idle.
enum class PendingClickIdleAction {
  DispatchSlowPath,
  BeginMarquee,
  WaitForMarqueeIntent,
};

[[nodiscard]] FrameMemorySample MemorySampleFromPresentationResources(
    const PresentationResourceStats& resources);
[[nodiscard]] FrameMissResourceTelemetry FrameMissTelemetryFromPresentationResources(
    const PresentationResourceStats& resources);
[[nodiscard]] ImGuiMouseCursor CursorForTransformHandleIntent(
    const SelectionTransformHandleIntent& intent);
[[nodiscard]] bool ContainsScreenPoint(const Box2d& rect, const ImVec2& point);
[[nodiscard]] PendingClickBusyAction PendingClickBusyActionForState(bool tookFastRedrag,
                                                                    bool rendererBusy);
[[nodiscard]] PendingClickIdleAction PendingClickIdleActionForState(
    bool leftMouseDown, bool pendingClickCanStartMarquee, bool selectHoldElapsed,
    bool selectDragIntent);
[[nodiscard]] css::RGBA PaintServerFallbackColor();
[[nodiscard]] ToolbarPaintSlotState ToolbarPaintSlotStateForActiveAttribute(std::string_view value);
[[nodiscard]] ToolbarPaintReferenceState ToolbarPaintReferenceStateFor(
    svg::SVGDocument* document, std::optional<std::string_view> source,
    const svg::Reference& reference);
[[nodiscard]] ToolbarPaintSlotState ToolbarPaintSlotStateForPaintServer(
    const svg::PaintServer& paint, const css::RGBA& currentColor, svg::SVGDocument* document,
    std::optional<std::string_view> source);
[[nodiscard]] ToolbarPaintState ToolbarPaintStateForActivePaint(const ActivePaintStyle& paintStyle);
[[nodiscard]] std::string PaintChipLabel(std::string_view prefix,
                                         const ToolbarPaintSlotState& state);
[[nodiscard]] std::string SelectionSizeChipLabel(const Box2d& screenBounds);
[[nodiscard]] std::string SelectionPositionChipLabel(const Box2d& documentBounds);
[[nodiscard]] std::string SelectionAngleChipLabel(const Transform2d& documentFromStartDocument);
[[nodiscard]] Box2d TransformDocumentBox(const Box2d& box,
                                         const Transform2d& documentFromBoundsDocument);
[[nodiscard]] float ClampSourcePaneWidthForWindow(float requestedWidth, float windowWidth);
[[nodiscard]] std::string ReferenceHighlightChipLabel(const ReferenceHighlightSummary& summary);
void AddUniqueElements(std::vector<svg::SVGElement>* target,
                       std::span<const svg::SVGElement> elements);
[[nodiscard]] bool ContainsElement(std::span<const svg::SVGElement> elements,
                                   const svg::SVGElement& element);
[[nodiscard]] std::string ElementContextMenuLabel(const svg::SVGElement& element);
[[nodiscard]] std::string InitialDocumentSyncSource(
    const ::donner::editor::EditorShellOptions& options);
[[nodiscard]] std::string CanonicalizeForTextEditor(std::string_view source);
[[nodiscard]] Box2d ResolveDocumentViewBox(svg::SVGDocument& document);

}  // namespace donner::editor::internal
