#pragma once
/**
 * @file DominantBaseline.h
 *
 * Defines the \ref donner::svg::DominantBaseline enum for the `dominant-baseline` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `dominant-baseline` property.
 *
 * Determines which baseline of the font is used to align the text.
 *
 * @see https://www.w3.org/TR/css-inline-3/#dominant-baseline-property
 */
enum class DominantBaseline : uint8_t {
  Auto,          ///< [DEFAULT] Use the default baseline for the script.
  TextBottom,    ///< Align to the bottom of the text.
  Alphabetic,    ///< Align to the alphabetic baseline.
  Ideographic,   ///< Align to the ideographic baseline.
  Middle,        ///< Align to the middle of the em box.
  Central,       ///< Align to the central baseline.
  Mathematical,  ///< Align to the mathematical baseline.
  Hanging,       ///< Align to the hanging baseline.
  TextTop,       ///< Align to the top of the text.
};

/**
 * Ostream output operator for \ref DominantBaseline enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, DominantBaseline value) {
  switch (value) {
    case DominantBaseline::Auto: return os << "auto";
    case DominantBaseline::TextBottom: return os << "text-bottom";
    case DominantBaseline::Alphabetic: return os << "alphabetic";
    case DominantBaseline::Ideographic: return os << "ideographic";
    case DominantBaseline::Middle: return os << "middle";
    case DominantBaseline::Central: return os << "central";
    case DominantBaseline::Mathematical: return os << "mathematical";
    case DominantBaseline::Hanging: return os << "hanging";
    case DominantBaseline::TextTop: return os << "text-top";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
