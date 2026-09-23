#pragma once
/// @file
/// Geometry, hit-testing, and drawing for the toolbar Fill/Stroke widget.
///
/// The widget presents the classic design-tool paired-swatch control: a solid
/// Fill swatch at upper left and a hollow Stroke swatch at lower right. The
/// active role comes to the foreground; one None control clears it. The pure
/// layout/hit-test/paint-string helpers here are unit tested; `EditorShell`
/// owns the imgui interaction wiring and the color-picker popups.

#include <string>

#include "donner/editor/EditorShellInternal.h"  // ToolbarPaintSlotState, ActivePaintStyle
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor::internal {

/// Interactive regions of the Fill/Stroke widget, in hit-test priority order.
enum class FillStrokeWidgetRegion {
  None,          ///< No interactive region under the point.
  FillSwatch,    ///< Fill swatch: activate Fill or open its color picker.
  StrokeSwatch,  ///< Stroke swatch: activate Stroke or open its color picker.
  Swap,          ///< Double-arrow affordance: swaps fill and stroke paints.
  SetNone,       ///< Clear the active paint role to `none`.
  FillChip,      ///< Fill custom-paint chip: reveals the paint-server source.
  StrokeChip,    ///< Stroke custom-paint chip: reveals the paint-server source.
};

/// Screen-space rectangles for every drawable/interactive part of the widget.
struct FillStrokeWidgetLayout {
  ImVec2 fillMin, fillMax;              ///< Fixed upper-left Fill swatch.
  ImVec2 strokeMin, strokeMax;          ///< Fixed lower-right Stroke swatch.
  ImVec2 swapMin, swapMax;              ///< Swap double-arrow affordance.
  ImVec2 noneMin, noneMax;              ///< One active-role "set none" affordance.
  ImVec2 fillChipMin, fillChipMax;      ///< Fill custom-paint label chip.
  ImVec2 strokeChipMin, strokeChipMax;  ///< Stroke custom-paint label chip.
};

/// Stable interaction policy for one fill/stroke toolbar frame.
struct FillStrokeWidgetInteractionState {
  /// Whether widget actions may mutate the active paint.
  bool canEdit = false;
  /// Whether the shell should replace its cached paint snapshot this frame.
  bool refreshPaintSnapshot = false;
};

/**
 * Resolve editability and snapshot refresh without keying drag chrome to a renderer busy bit.
 *
 * The caller reports whether its snapshot belongs to the current document and first selected
 * element, not merely whether any snapshot exists.
 *
 * @param hasDocument Whether the editor currently owns a document.
 * @param rendererBusy Whether the async renderer currently owns document access.
 * @param canvasInteractionActive Whether a canvas gesture is in progress.
 * @param paintSnapshotMatchesSelection Whether the cached paint belongs to the current selection.
 * @return Editability and snapshot-refresh policy for the current toolbar frame.
 */
[[nodiscard]] FillStrokeWidgetInteractionState ResolveFillStrokeWidgetInteractionState(
    bool hasDocument, bool rendererBusy, bool canvasInteractionActive,
    bool paintSnapshotMatchesSelection);

/// Compute widget sub-rectangles from the widget's outer bounds.
[[nodiscard]] FillStrokeWidgetLayout ComputeFillStrokeWidgetLayout(const ImVec2& widgetMin,
                                                                   const ImVec2& widgetMax);

/// Classify @p point against @p layout. The active swatch wins overlap. Chip
/// regions only match when the corresponding slot carries custom paint.
[[nodiscard]] FillStrokeWidgetRegion HitTestFillStrokeWidget(const FillStrokeWidgetLayout& layout,
                                                             const ImVec2& point, bool fillIsCustom,
                                                             bool strokeIsCustom,
                                                             bool fillIsActive);

/// Reconstruct an SVG paint attribute string ("none", "#rrggbb", "url(#id)",
/// "context-fill", ...) from a resolved paint slot. Used by the swap action.
[[nodiscard]] std::string SvgPaintStringForSlot(const ToolbarPaintSlotState& slot);

/// Swap the fill and stroke of an active paint style in place.
void SwapActivePaint(ActivePaintStyle& style);

/// Draw a paint swatch. @p fillRole determines the solid Fill versus hollow
/// Stroke motif, independently of @p active foreground status. Handles the
/// none (red slash) and custom (diagonal hatch) presentations.
void DrawFillStrokeSwatch(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                          const ToolbarPaintSlotState& state, bool fillRole, bool active);

/// Draw the swap double-arrow affordance.
void DrawSwapAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool enabled);

/// Draw the single "set none" affordance; @p alreadyNone brightens it when
/// the active role is already none.
void DrawNoneAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                        bool alreadyNone);

/// Draw both fixed paint roles in foreground order with their shared controls
/// and optional custom-paint chips.
void DrawFillStrokeWidget(ImDrawList* drawList, const FillStrokeWidgetLayout& layout,
                          const ToolbarPaintState& paintState, bool fillIsActive, bool canEdit);

}  // namespace donner::editor::internal
