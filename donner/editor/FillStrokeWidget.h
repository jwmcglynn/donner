#pragma once
/// @file
/// Geometry, hit-testing, and drawing for the toolbar Fill/Stroke widget.
///
/// The widget presents the classic design-tool paired-swatch control: a filled
/// "fill" swatch in front and a hollow "stroke" swatch behind, with per-slot
/// affordances to clear a slot to `none` and to swap fill and stroke. The pure
/// layout/hit-test/paint-string helpers here are unit tested; `EditorShell`
/// owns the imgui interaction wiring and the color-picker popups.

#include <string>

#include "donner/editor/EditorShellInternal.h"  // ToolbarPaintSlotState, ActivePaintStyle
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor::internal {

/// Interactive regions of the Fill/Stroke widget, in hit-test priority order.
enum class FillStrokeWidgetRegion {
  None,         ///< No interactive region under the point.
  FillSwatch,   ///< Front (fill) swatch: opens the fill color picker.
  StrokeSwatch,  ///< Back (stroke) swatch: opens the stroke color picker.
  Swap,          ///< Double-arrow affordance: swaps fill and stroke paints.
  FillNone,      ///< Fill "set none" affordance.
  StrokeNone,    ///< Stroke "set none" affordance.
  FillChip,      ///< Fill custom-paint chip: reveals the paint-server source.
  StrokeChip,    ///< Stroke custom-paint chip: reveals the paint-server source.
};

/// Screen-space rectangles for every drawable/interactive part of the widget.
struct FillStrokeWidgetLayout {
  ImVec2 fillMin, fillMax;              ///< Front (fill) swatch.
  ImVec2 strokeMin, strokeMax;         ///< Back (stroke) swatch.
  ImVec2 swapMin, swapMax;             ///< Swap double-arrow affordance.
  ImVec2 fillNoneMin, fillNoneMax;     ///< Fill "set none" affordance.
  ImVec2 strokeNoneMin, strokeNoneMax;  ///< Stroke "set none" affordance.
  ImVec2 fillChipMin, fillChipMax;     ///< Fill custom-paint label chip.
  ImVec2 strokeChipMin, strokeChipMax;  ///< Stroke custom-paint label chip.
};

/// Compute widget sub-rectangles from the widget's outer bounds.
[[nodiscard]] FillStrokeWidgetLayout ComputeFillStrokeWidgetLayout(const ImVec2& widgetMin,
                                                                   const ImVec2& widgetMax);

/// Classify @p point against @p layout. Chip regions only match when the
/// corresponding slot carries custom paint (only then is a chip drawn).
[[nodiscard]] FillStrokeWidgetRegion HitTestFillStrokeWidget(const FillStrokeWidgetLayout& layout,
                                                             const ImVec2& point, bool fillIsCustom,
                                                             bool strokeIsCustom);

/// Reconstruct an SVG paint attribute string ("none", "#rrggbb", "url(#id)",
/// "context-fill", ...) from a resolved paint slot. Used by the swap action.
[[nodiscard]] std::string SvgPaintStringForSlot(const ToolbarPaintSlotState& slot);

/// Swap the fill and stroke of an active paint style in place.
void SwapActivePaint(ActivePaintStyle& style);

/// Draw a paint swatch. @p front draws a solid filled swatch (fill role); a
/// non-front swatch draws a hollow ring (stroke role). Handles the none (red
/// slash) and custom (diagonal hatch) presentations.
void DrawFillStrokeSwatch(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                          const ToolbarPaintSlotState& state, bool front);

/// Draw the swap double-arrow affordance.
void DrawSwapAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool enabled);

/// Draw a "set none" affordance. @p fillVariant selects the solid (fill) motif
/// versus the hollow (stroke) motif; @p active brightens it when the slot is
/// already none.
void DrawNoneAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool fillVariant,
                        bool active);

}  // namespace donner::editor::internal
