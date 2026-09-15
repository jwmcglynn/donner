#pragma once
/**
 * @file Visibility.h
 *
 * Defines the \ref donner::svg::Visibility enum, which is used to determine whether an element is
 * visible or hidden.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the 'visibility' property, see:
 * https://www.w3.org/TR/CSS2/visufx.html#propdef-visibility
 *
 * This determines whether the element is painted. In SVG both non-visible values suppress painting
 * only: `collapse` has its CSS table-row meaning nowhere in SVG, so for text content it behaves as
 * `hidden`, and a span with either value is still laid out, still advances the pen for the spans
 * that follow it, and still contributes to the element's object bounding box.
 */
enum class Visibility : uint8_t {
  Visible,   ///< [DEFAULT] Visible is the default value.
  Hidden,    ///< Hidden elements are not painted, but are still laid out.
  Collapse,  ///< Behaves as \ref Visibility::Hidden for SVG content.
};

/**
 * Ostream output operator for \ref Visibility enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, Visibility value) {
  switch (value) {
    case Visibility::Visible: return os << "visible";
    case Visibility::Hidden: return os << "hidden";
    case Visibility::Collapse: return os << "collapse";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
