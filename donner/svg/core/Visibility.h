#pragma once
/**
 * @file Visibility.h
 *
 * Defines the \ref donner::svg::Visibility enum, which is used to determine whether an element is
 * visible or hidden.
 */

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the 'visibility' property, see:
 * https://www.w3.org/TR/CSS2/visufx.html#propdef-visibility
 *
 * This determines whether the element is visible or hidden, and whether it affects layout.
 */
enum class Visibility : uint8_t {
  Visible,   ///< [DEFAULT] Visible is the default value.
  Hidden,    ///< Hidden elements are invisible, but still affect layout.
  Collapse,  ///< Collapsed elements are invisible, and do not affect layout.
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

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
