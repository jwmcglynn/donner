#pragma once
/// @file

#include <optional>
#include <string_view>

#include "donner/base/Length.h"

namespace donner::editor {

struct EditorTheme;

/**
 * Parse a length typed into an Inspector field.
 *
 * Accepts a CSS `<length-percentage>` such as `1in`, `2.5mm` or `50%`, using
 * \ref donner::parser::LengthParser, or a bare number. A bare number takes \p defaultUnit, so
 * editing a value keeps the unit it was authored in; a typed unit replaces it. Surrounding
 * whitespace is ignored. Anything else, including an unknown unit or a space before the unit, is
 * rejected rather than truncated to its leading number.
 *
 * @param text Text typed by the user.
 * @param defaultUnit Unit for a bare number.
 * @return The typed length, or std::nullopt if \p text is not a finite length.
 */
[[nodiscard]] std::optional<Lengthd> ParseInspectorLength(std::string_view text,
                                                          LengthUnit defaultUnit);

/**
 * Parse a plain number typed into an Inspector field; a unit suffix is rejected, not dropped.
 *
 * @param text Text typed by the user.
 * @return The number, or std::nullopt if \p text is not a single finite number.
 */
[[nodiscard]] std::optional<double> ParseInspectorNumber(std::string_view text);

/**
 * Parse a rotation typed into an Inspector field: a number of degrees, optionally followed by the
 * `deg` unit the field displays. Other units are rejected, not dropped.
 *
 * @param text Text typed by the user.
 * @return The angle in degrees, or std::nullopt if \p text is not a finite angle in degrees.
 */
[[nodiscard]] std::optional<double> ParseInspectorDegrees(std::string_view text);

/// Tooltip shown while a length field's text is rejected.
inline constexpr const char* kInspectorLengthHint =
    "Enter a number, optionally with a unit such as px, in, cm, mm, pt, pc, em, ex or %";
/// Tooltip shown while a plain-number field's text is rejected.
inline constexpr const char* kInspectorNumberHint = "Enter a number without a unit";
/// Tooltip shown while a rotation field's text is rejected.
inline constexpr const char* kInspectorDegreesHint = "Enter an angle in degrees";

/// Converts the typed text of a numeric field to its value, or returns std::nullopt to reject it.
struct InspectorTextParser {
  /// Parse function, called with \ref context and the typed text.
  std::optional<double> (*parse)(const void* context, std::string_view text) = nullptr;
  const void* context = nullptr;  ///< Caller state passed to \ref parse.
};

/// Outcome of one frame of an Inspector numeric field.
struct InspectorNumericEdit {
  bool changed = false;   ///< The field's value changed this frame.
  bool typing = false;    ///< The frame began in text entry, so `rejected` is meaningful.
  bool rejected = false;  ///< The typed text cannot be interpreted; the value was left unchanged.
};

/**
 * Draggable numeric field whose typed text is interpreted by \p parse.
 *
 * Behaves like `ImGui::DragScalar` over a double, except that text typed into the field (after a
 * click, double-click or Ctrl+click) is interpreted by \p parse. ImGui's own parsing keeps only the
 * leading number and silently drops anything after it, such as a unit. Text that \p parse rejects
 * leaves the value unchanged and outlines the field in the theme's destructive color, with
 * \p hint as its tooltip. Text the user has not changed is not re-parsed, so entering and leaving
 * the field does not round the value to its displayed precision.
 *
 * @param label ImGui label and ID of the field.
 * @param value Value to display and edit.
 * @param speed Drag speed, in value units per pixel.
 * @param format printf-style display format.
 * @param parse Interprets typed text.
 * @param hint Tooltip explaining what the field accepts, shown while its text is rejected.
 * @param theme Active editor theme.
 * @return Whether the value changed and whether the typed text is currently rejected.
 */
InspectorNumericEdit DragInspectorNumber(const char* label, double* value, float speed,
                                         const char* format, const InspectorTextParser& parse,
                                         const char* hint, const EditorTheme& theme);

/// Outcome of one frame of an Inspector length field.
struct InspectorLengthEdit {
  std::optional<Lengthd> typed;  ///< Valid typed length that differs from the displayed value.
  bool typing = false;           ///< This frame had the field's typed text, so `rejected` applies.
  bool rejected = false;         ///< The typed text cannot be interpreted.
};

/**
 * Text field for a CSS length, displayed with one decimal place and its unit, e.g. `2.0mm`.
 *
 * Typed text is interpreted by \ref ParseInspectorLength with \p bareNumberUnit as the default,
 * so a bare number keeps the authored unit and a typed unit replaces it. Rejected text outlines the
 * field like \ref DragInspectorNumber.
 *
 * @param label ImGui label and ID of the field.
 * @param current Value displayed while the field is not being edited.
 * @param bareNumberUnit Unit for a typed bare number, normally the unit before editing began.
 * @param allowNegative Whether negative lengths are accepted; otherwise they are rejected.
 * @param theme Active editor theme.
 * @return The newly typed length, if any, and whether the typed text is currently rejected.
 */
InspectorLengthEdit InputInspectorLength(const char* label, const Lengthd& current,
                                         LengthUnit bareNumberUnit, bool allowNegative,
                                         const EditorTheme& theme);

}  // namespace donner::editor
