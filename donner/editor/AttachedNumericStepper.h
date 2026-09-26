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

}  // namespace donner::editor
