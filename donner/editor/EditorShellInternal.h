#pragma once
/// @file

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/CanvasScrollbars.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/FocusView.h"
#include "donner/editor/FrameMissTelemetry.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/PenTool.h"
#include "donner/editor/RotateCursorSet.h"
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

/// End-of-frame action for a deferred document render.
enum class DeferredRenderAction {
  ClearRequest,
  WakeForPenDrag,
  SubmitRender,
  WaitForRendererCompletion,
};

[[nodiscard]] FrameMemorySample MemorySampleFromPresentationResources(
    const PresentationResourceStats& resources);
[[nodiscard]] FrameMissResourceTelemetry FrameMissTelemetryFromPresentationResources(
    const PresentationResourceStats& resources);
[[nodiscard]] ImGuiMouseCursor CursorForTransformHandleIntent(
    const SelectionTransformHandleIntent& intent);

/// Pen nib variant to show for @p intent.
///
/// \ref PenHoverIntent::DragAnchor has no nib: grabbing an existing anchor is
/// not a nib gesture, so the shell shows the anchor-point cursor for it and
/// this returns the plain nib as an inert default.
///
/// @param intent What a pen-tool click under the pointer would do.
[[nodiscard]] PenCursorHint PenCursorHintForIntent(PenHoverIntent intent);
[[nodiscard]] bool ContainsScreenPoint(const Box2d& rect, const ImVec2& point);
[[nodiscard]] std::optional<Box2d> TextFormatBarScreenRect(const ImVec2& paneOrigin,
                                                           const ImVec2& contentRegion,
                                                           const Box2d& toolPaletteRect,
                                                           bool visible, float barHeight);
[[nodiscard]] bool CanvasChromeCapturesInput(
    const ImVec2& point, const std::optional<Box2d>& referenceChipRect,
    const Box2d& toolPaletteRect, const std::optional<Box2d>& textFormatBarRect,
    const std::optional<Box2d>& editingScopeBreadcrumbRect, const Box2d& canvasZoomControlRect,
    const std::optional<Box2d>& compactPanelRect = std::nullopt);
[[nodiscard]] bool CanvasScrollbarsCaptureInput(bool scrollbarsVisible,
                                                const ViewportState& viewport,
                                                const Vector2d& screenPoint) noexcept;
[[nodiscard]] bool GroupOperationCanDispatch(
    bool rendererBusy, const GroupOperationAvailability& availability) noexcept;
[[nodiscard]] bool PendingDocumentReplacementCanProcess(bool hasPendingRequest,
                                                        bool documentWriteAvailable,
                                                        bool hasPendingMutations) noexcept;
[[nodiscard]] bool ShouldRefreshSidebarSnapshots(bool rendererBusy,
                                                 bool interactionActive) noexcept;
[[nodiscard]] bool SidebarSnapshotRefreshPendingAfterPass(
    std::size_t deferredThumbnailCount) noexcept;

/// Resolve a document-derived UI boolean without ever entering the live document while the
/// renderer owns it. Busy frames replay the value from the last complete UI epoch.
template <typename Resolver>
[[nodiscard]] bool ResolveCachedDocumentBoolForFrame(bool rendererBusy, bool cachedValue,
                                                     Resolver&& resolveWhenIdle) {
  if (rendererBusy) {
    return cachedValue;
  }
  return static_cast<bool>(std::forward<Resolver>(resolveWhenIdle)());
}

[[nodiscard]] bool SamplePickerActionsNeedFollowupFrame(bool dismiss, bool openFile) noexcept;
[[nodiscard]] DeferredRenderAction DeferredRenderActionForState(bool hasDocument,
                                                                bool penDragFlushed,
                                                                bool rendererBusy) noexcept;
[[nodiscard]] PendingClickBusyAction PendingClickBusyActionForState(bool tookFastRedrag,
                                                                    bool documentWriteUnavailable);
[[nodiscard]] PendingClickIdleAction PendingClickIdleActionForState(
    bool leftMouseDown, bool pendingClickCanStartMarquee, bool selectHoldElapsed,
    bool selectDragIntent);
/// Discoverability hint for the idle text tool ("double-click places point
/// text, drag draws a box"). Empty while a session or box drag is active -
/// the hint only shows when the next click/drag would act on empty canvas.
[[nodiscard]] std::string_view TextToolHintLabel(bool isEditing, bool isDraggingBox,
                                                 bool touchPreferred = false);
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
[[nodiscard]] std::optional<Box2d> ResolveDocumentViewBoxForFrame(svg::SVGDocument& document,
                                                                  bool rendererBusy);

}  // namespace donner::editor::internal
