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
  std::string href;                    //!< Authored paint-server reference.
  std::optional<css::Color> fallback;  //!< Unresolved authored fallback, including currentColor.
  bool external = false;  //!< Whether the reference points outside the current document.
  std::optional<SourceByteRange>
      sourceRange;  //!< Authored byte range of the reference, when available.
};

/// Paint slot presentation state for the fill/stroke toolbar.
struct ToolbarPaintSlotState {
  css::RGBA color =
      css::RGBA::RGB(0, 0, 0);  //!< Resolved solid color displayed by the paint swatch.
  bool isNone = true;           //!< Whether the slot represents no paint.
  bool isCustom =
      false;  //!< Whether the slot requires a custom-paint label instead of a solid swatch.
  std::optional<ToolbarPaintReferenceState>
      reference;            //!< Referenced paint-server state, when the slot uses a reference.
  std::string customLabel;  //!< Display label for paint not represented by a solid swatch.
};

/// Fill and stroke toolbar paint state.
struct ToolbarPaintState {
  ToolbarPaintSlotState fill;    //!< Current fill-slot presentation.
  ToolbarPaintSlotState stroke;  //!< Current stroke-slot presentation.
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

/// Copy presentation resource counters into a frame-history sample.
/// @param resources Current presentation resource accounting.
[[nodiscard]] FrameMemorySample MemorySampleFromPresentationResources(
    const PresentationResourceStats& resources);

/// Copy presentation resource counters into frame-miss telemetry.
/// @param resources Current presentation resource accounting.
[[nodiscard]] FrameMissResourceTelemetry FrameMissTelemetryFromPresentationResources(
    const PresentationResourceStats& resources);

/// Select the ImGui cursor for a transform-handle hit.
/// @param intent Transform action and corner under the pointer.
[[nodiscard]] ImGuiMouseCursor CursorForTransformHandleIntent(
    const SelectionTransformHandleIntent& intent);

/// Pen nib variant to show for @p intent.
///
/// \ref donner::editor::PenHoverIntent::DragAnchor "PenHoverIntent::DragAnchor" has no nib:
/// grabbing an existing anchor is not a nib gesture, so the shell shows the anchor-point cursor for
/// it and this returns the plain nib as an inert default.
///
/// @param intent What a pen-tool click under the pointer would do.
[[nodiscard]] PenCursorHint PenCursorHintForIntent(PenHoverIntent intent);

/// Test inclusive containment in a screen-space rectangle.
/// @param rect Rectangle in logical screen pixels.
/// @param point Point in the same coordinate system.
[[nodiscard]] bool ContainsScreenPoint(const Box2d& rect, const ImVec2& point);

/// Place the format bar below the tool palette, or return no rectangle when hidden or empty.
/// @param paneOrigin Render-pane origin in logical screen pixels.
/// @param contentRegion Available pane size in logical pixels.
/// @param toolPaletteRect Screen-space toolbar bounds used to align the format bar.
/// @param visible Whether the format bar is requested.
/// @param barHeight Requested height in logical pixels.
[[nodiscard]] std::optional<Box2d> TextFormatBarScreenRect(const ImVec2& paneOrigin,
                                                           const ImVec2& contentRegion,
                                                           const Box2d& toolPaletteRect,
                                                           bool visible, float barHeight);

/// Test whether a pointer lies in any visible canvas chrome control.
/// @param point Pointer position in logical screen pixels.
/// @param referenceChipRect Optional reference-chip bounds.
/// @param toolPaletteRect Tool-palette bounds.
/// @param textFormatBarRect Optional text-format bar bounds.
/// @param editingScopeBreadcrumbRect Optional editing-scope breadcrumb bounds.
/// @param canvasZoomControlRect Zoom-control bounds.
/// @param compactPanelRect Optional compact-panel bounds.
[[nodiscard]] bool CanvasChromeCapturesInput(
    const ImVec2& point, const std::optional<Box2d>& referenceChipRect,
    const Box2d& toolPaletteRect, const std::optional<Box2d>& textFormatBarRect,
    const std::optional<Box2d>& editingScopeBreadcrumbRect, const Box2d& canvasZoomControlRect,
    const std::optional<Box2d>& compactPanelRect = std::nullopt);

/// Test whether visible canvas scrollbars capture a screen-space pointer.
/// @param scrollbarsVisible Whether scrollbars are currently shown.
/// @param viewport Viewport used to compute scrollbar geometry.
/// @param screenPoint Pointer position in logical screen pixels.
[[nodiscard]] bool CanvasScrollbarsCaptureInput(bool scrollbarsVisible,
                                                const ViewportState& viewport,
                                                const Vector2d& screenPoint) noexcept;

/// Permit a group command only when the renderer is idle and the operation is available.
/// @param rendererBusy Whether the worker currently owns the document.
/// @param availability Precomputed availability of the requested group operation.
[[nodiscard]] bool GroupOperationCanDispatch(
    bool rendererBusy, const GroupOperationAvailability& availability) noexcept;

/// Permit a queued replacement only when document writes are available and mutations are drained.
/// @param hasPendingRequest Whether a replacement is queued.
/// @param documentWriteAvailable Whether the caller can write the document now.
/// @param hasPendingMutations Whether earlier mutations still need processing.
[[nodiscard]] bool PendingDocumentReplacementCanProcess(bool hasPendingRequest,
                                                        bool documentWriteAvailable,
                                                        bool hasPendingMutations) noexcept;

/// Refresh sidebar captures only while both rendering and interaction are idle.
/// @param rendererBusy Whether the renderer owns the document.
/// @param interactionActive Whether an interaction is in progress.
[[nodiscard]] bool ShouldRefreshSidebarSnapshots(bool rendererBusy,
                                                 bool interactionActive) noexcept;

/// Report whether deferred thumbnails require another snapshot refresh.
/// @param deferredThumbnailCount Number of thumbnails left pending by this pass.
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

/// Request another frame when the picker dismisses, opens a file, or creates a document.
/// @param dismiss Whether the picker was dismissed.
/// @param openFile Whether file opening was requested.
/// @param newDocument Whether a new document was requested.
[[nodiscard]] bool SamplePickerActionsNeedFollowupFrame(bool dismiss, bool openFile,
                                                        bool newDocument) noexcept;
/// Background carousel work yields while a selected sample owns the next document render.
[[nodiscard]] bool ShouldAdvanceSampleThumbnails(bool showSamplePicker,
                                                 bool samplePresentationPending) noexcept;

/// Choose how to advance a deferred render request from the current document and worker state.
/// @param hasDocument Whether a document is loaded.
/// @param penDragFlushed Whether this pass applied a pending pen drag.
/// @param rendererBusy Whether the render worker is busy.
[[nodiscard]] DeferredRenderAction DeferredRenderActionForState(bool hasDocument,
                                                                bool penDragFlushed,
                                                                bool rendererBusy) noexcept;

/// Choose the buffered-click action while the renderer owns the document.
/// @param tookFastRedrag Whether the cached presentation accepted a fast redrag.
/// @param documentWriteUnavailable Whether live document mutation is unavailable.
[[nodiscard]] PendingClickBusyAction PendingClickBusyActionForState(bool tookFastRedrag,
                                                                    bool documentWriteUnavailable);

/// Choose the buffered-click action after the renderer releases the document.
/// @param leftMouseDown Whether the initiating mouse button remains held.
/// @param pendingClickCanStartMarquee Whether the buffered click may start a marquee.
/// @param selectHoldElapsed Whether the selection hold interval elapsed.
/// @param selectDragIntent Whether pointer movement establishes a selection drag.
[[nodiscard]] PendingClickIdleAction PendingClickIdleActionForState(
    bool leftMouseDown, bool pendingClickCanStartMarquee, bool selectHoldElapsed,
    bool selectDragIntent);
/// Discoverability hint for the idle text tool ("double-click places point
/// text, drag draws a box"). Empty while a session or box drag is active -
/// the hint only shows when the next click/drag would act on empty canvas.
[[nodiscard]] std::string_view TextToolHintLabel(bool isEditing, bool isDraggingBox,
                                                 bool touchPreferred = false);

/// Return the neutral toolbar color used for paint that lacks a solid-color preview.
[[nodiscard]] css::RGBA PaintServerFallbackColor();

/// Build a toolbar swatch from an authored paint value, retaining custom values as labels.
/// @param value Authored fill or stroke value.
[[nodiscard]] ToolbarPaintSlotState ToolbarPaintSlotStateForActiveAttribute(std::string_view value);

/// Capture a paint reference and its available source location for the toolbar.
/// @param document Document used to resolve local references, or null.
/// @param source Synchronized source text when available.
/// @param reference Authored paint-server reference.
[[nodiscard]] ToolbarPaintReferenceState ToolbarPaintReferenceStateFor(
    svg::SVGDocument* document, std::optional<std::string_view> source,
    const svg::Reference& reference);

/// Build a toolbar swatch and reference metadata from resolved paint.
/// @param paint Paint server to display.
/// @param currentColor Color used to resolve currentColor paint.
/// @param document Document used to inspect local references, or null.
/// @param source Synchronized source text when available.
[[nodiscard]] ToolbarPaintSlotState ToolbarPaintSlotStateForPaintServer(
    const svg::PaintServer& paint, const css::RGBA& currentColor, svg::SVGDocument* document,
    std::optional<std::string_view> source);

/// Build fill and stroke toolbar presentation from the active paint style.
/// @param paintStyle Active fill and stroke values.
[[nodiscard]] ToolbarPaintState ToolbarPaintStateForActivePaint(const ActivePaintStyle& paintStyle);

/// Format the label of a toolbar paint chip.
/// @param prefix Role label, such as fill or stroke.
/// @param state Paint-slot presentation to describe.
[[nodiscard]] std::string PaintChipLabel(std::string_view prefix,
                                         const ToolbarPaintSlotState& state);

/// Format the selection bounds width and height for a toolbar chip.
/// @param screenBounds Bounds whose dimensions are displayed.
[[nodiscard]] std::string SelectionSizeChipLabel(const Box2d& screenBounds);

/// Format the document-space selection position for a toolbar chip.
/// @param documentBounds Selection bounds in document coordinates.
[[nodiscard]] std::string SelectionPositionChipLabel(const Box2d& documentBounds);

/// Format the rotation represented by a selection transform.
/// @param documentFromStartDocument Transform from gesture-start document coordinates to current
/// coordinates.
[[nodiscard]] std::string SelectionAngleChipLabel(const Transform2d& documentFromStartDocument);

/// Transform every box corner and return their axis-aligned document-space bounds.
/// @param box Bounds in the source coordinate system.
/// @param documentFromBoundsDocument Transform into destination document coordinates.
[[nodiscard]] Box2d TransformDocumentBox(const Box2d& box,
                                         const Transform2d& documentFromBoundsDocument);

/// Clamp a requested source-pane width to the usable window range.
/// @param requestedWidth Requested width in logical pixels.
/// @param windowWidth Available window width in logical pixels.
[[nodiscard]] float ClampSourcePaneWidthForWindow(float requestedWidth, float windowWidth);

/// Format the toolbar label for the current reference-highlight summary.
/// @param summary Reference counts and highlight state.
[[nodiscard]] std::string ReferenceHighlightChipLabel(const ReferenceHighlightSummary& summary);

/// Append elements not already present in a destination list.
/// @param target Non-null destination list to update.
/// @param elements Elements to append without duplicates.
void AddUniqueElements(std::vector<svg::SVGElement>* target,
                       std::span<const svg::SVGElement> elements);

/// Test whether an element handle is present in a list.
/// @param elements Handles to search.
/// @param element Handle to find.
[[nodiscard]] bool ContainsElement(std::span<const svg::SVGElement> elements,
                                   const svg::SVGElement& element);

/// Build the display label for an element in the context menu.
/// @param element Element whose tag and identity are displayed.
[[nodiscard]] std::string ElementContextMenuLabel(const svg::SVGElement& element);

/// Use host-provided initial source, otherwise load the requested file or return empty text.
/// @param options Initial document source and path options.
[[nodiscard]] std::string InitialDocumentSyncSource(
    const ::donner::editor::EditorShellOptions& options);

/// Remove one final newline to match the text editor source representation.
/// @param source Source text to copy and normalize.
[[nodiscard]] std::string CanonicalizeForTextEditor(std::string_view source);

/// Resolve the authored viewBox, intrinsic dimensions, canvas size, or unit-box fallback under read
/// access.
/// @param document Document whose root geometry is inspected.
[[nodiscard]] Box2d ResolveDocumentViewBox(svg::SVGDocument& document);

/// Return no update while rendering is busy; otherwise resolve the document viewBox.
/// @param document Document whose root geometry is inspected while idle.
/// @param rendererBusy Whether the worker currently owns the document.
[[nodiscard]] std::optional<Box2d> ResolveDocumentViewBoxForFrame(svg::SVGDocument& document,
                                                                  bool rendererBusy);

}  // namespace donner::editor::internal
