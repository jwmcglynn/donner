#pragma once
/// @file

#include "donner/base/Box.h"

struct ImVec2;

namespace donner::editor {

struct EditorTheme;

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

/// Open a preset popup beneath a focused numeric input without stealing its typing focus.
///
/// Call after rendering the input and its attached stepper. The caller must pair a true return
/// with ImGui::EndPopup(). Single-click activation keeps the field editable while expanding the
/// preset choices.
///
/// @param popupId Stable ImGui popup ID in the caller's current ID scope.
/// @param fieldMin Minimum screen-space corner of the numeric input.
/// @param fieldMax Maximum screen-space corner of the numeric input.
/// @param fieldActivated True only on the input's focus-acquisition frame.
/// @param width Popup width in logical pixels.
/// @param maximumHeight Maximum popup height before its contents scroll.
/// @return True when the popup is open and BeginPopup succeeded.
[[nodiscard]] bool BeginHybridNumericPresetPopup(const char* popupId, const ImVec2& fieldMin,
                                                 const ImVec2& fieldMax, bool fieldActivated,
                                                 float width, float maximumHeight);

}  // namespace donner::editor
