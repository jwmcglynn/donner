#pragma once
/// @file

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Values for the `"lengthAdjust"` attribute,
 * https://www.w3.org/TR/SVG2/text.html#TextElementLengthAdjustAttribute.
 *
 * This is used on the \ref xml_text and \ref xml_tspan elements, and controls how the text is
 * stretched, either by adding spacing between glyphs or also by stretching the glyphs themselves.
 */
enum class LengthAdjust {
  /**
   * The text is stretched by adding spacing between glyphs, but the glyphs themselves are not
   * scaled.
   */
  Spacing,
  /**
   * The text is stretched by adding spacing, and the glyphs are also stretched or compressed to
   * fit the `textLength`.
   */
  SpacingAndGlyphs,
  /**
   * The default value for the `"lengthAdjust"` attribute, which is `spacing`.
   */
  Default = Spacing,
};

/// Ostream output operator for \ref LengthAdjust enum, outputs the enum name with prefix, e.g.
/// `LengthAdjust::Spacing`.
inline std::ostream& operator<<(std::ostream& os, LengthAdjust units) {
  switch (units) {
    case LengthAdjust::Spacing: return os << "LengthAdjust::Spacing";
    case LengthAdjust::SpacingAndGlyphs: return os << "LengthAdjust::SpacingAndGlyphs";
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
