#pragma once
/**
 * @file TextAnchor.h
 *
 * Defines the \ref donner::svg::TextAnchor enum for the `text-anchor` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `text-anchor` property.
 *
 * Determines the alignment of text relative to its anchor position (the x/y coordinates).
 *
 * @see https://www.w3.org/TR/SVG2/text.html#TextAnchorProperty
 */
enum class TextAnchor : uint8_t {
  Start,   ///< [DEFAULT] Text starts at the anchor position.
  Middle,  ///< Text is centered on the anchor position.
  End,     ///< Text ends at the anchor position.
};

/**
 * Ostream output operator for \ref TextAnchor enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, TextAnchor value) {
  switch (value) {
    case TextAnchor::Start: return os << "start";
    case TextAnchor::Middle: return os << "middle";
    case TextAnchor::End: return os << "end";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
