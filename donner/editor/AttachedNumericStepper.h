#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"

struct ImVec2;

namespace donner::editor {

struct EditorTheme;

/// Logical width of the stepper pair attached to a numeric input.
inline constexpr float kAttachedNumericStepperWidth = 22.0f;

/// One pair of compact step buttons joined to the right edge of a numeric field.
struct AttachedNumericStepperResult {
  bool increment = false;  ///< Up button was activated in this frame.
  bool decrement = false;  ///< Down button was activated in this frame.
  Box2d incrementRect;     ///< Screen-space hit box of the upper half.
  Box2d decrementRect;     ///< Screen-space hit box of the lower half.
};

/// Draw two vertically stacked step buttons with the exact height of the preceding field.
///
/// Call immediately after an ImGui numeric input. The pair has no horizontal gap, shares a
/// single outer frame with the input, and keeps its hit boxes independent of the field's value.
///
/// @param id Stable ImGui ID scope for this pair.
/// @param fieldMin Minimum screen-space corner of the preceding numeric field.
/// @param fieldMax Maximum screen-space corner of the preceding numeric field.
/// @param theme Active editor colors and corner radius.
/// @return Activated step plus both screen-space hit boxes.
[[nodiscard]] AttachedNumericStepperResult RenderAttachedNumericStepper(const char* id,
                                                                        const ImVec2& fieldMin,
                                                                        const ImVec2& fieldMax,
                                                                        const EditorTheme& theme);

/// Where a numeric preset popup opens relative to the control it belongs to.
struct NumericPresetPopupPlacement {
  /// Screen-space anchor: the popup's top-left corner when it opens below the control, or its
  /// bottom-left corner when it opens above.
  Vector2d anchor;
  bool opensAbove = false;     ///< True when the popup opens above the control.
  double maximumHeight = 0.0;  ///< Tallest popup that fits on the chosen side; contents scroll.
};

/**
 * Choose a preset popup position that never overlaps the control it belongs to.
 *
 * The popup opens below the control when its preferred height fits there, otherwise above when it
 * fits there, otherwise on the side with more room with its height reduced so the list scrolls.
 * Horizontally it follows the control's left edge, shifted only to stay inside the bounds when
 * it fits within them.
 *
 * @param control Screen-space rectangle of the numeric field plus its attached stepper.
 * @param preferredHeight Height the popup needs to show all choices, capped by the caller's limit.
 * @param width Popup width.
 * @param bounds Screen-space rectangle the popup must stay within.
 * @param gap Vertical spacing kept between the control and the popup.
 */
[[nodiscard]] NumericPresetPopupPlacement PlaceNumericPresetPopup(const Box2d& control,
                                                                  double preferredHeight,
                                                                  double width, const Box2d& bounds,
                                                                  double gap);

/// Open a preset popup next to a focused numeric input without stealing its typing focus.
///
/// Call after rendering the input and its attached stepper. The caller must pair a true return
/// with ImGui::EndPopup(). Single-click activation keeps the field editable while expanding the
/// preset choices. Its width matches the numeric field plus attached stepper. It opens below the
/// field when the list fits there, otherwise above, and never covers the field (see
/// \ref donner::editor::PlaceNumericPresetPopup).
///
/// @param popupId Stable ImGui popup ID in the caller's current ID scope.
/// @param fieldMin Minimum screen-space corner of the numeric input.
/// @param fieldMax Maximum screen-space corner of the numeric input.
/// @param fieldActivated True only on the input's focus-acquisition frame.
/// @param maximumHeight Maximum popup height before its contents scroll.
/// @return True when the popup is open and BeginPopup succeeded.
[[nodiscard]] bool BeginHybridNumericPresetPopup(const char* popupId, const ImVec2& fieldMin,
                                                 const ImVec2& fieldMax, bool fieldActivated,
                                                 float maximumHeight);

}  // namespace donner::editor
