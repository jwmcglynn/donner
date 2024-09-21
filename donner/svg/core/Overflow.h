#pragma once
/**
 * @file Overflow.h
 *
 * Defines the \ref donner::svg::Overflow enum, which is used to determine how an element handles
 * content that is too large for its container.
 */

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the CSS 'overflow' property, see
 * https://www.w3.org/TR/CSS2/visufx.html#overflow and
 * https://www.w3.org/TR/SVG2/render.html#OverflowAndClipProperties.
 *
 * The 'overflow' property specifies whether to clip content, render scrollbars, or display content
 * outside the element's box.
 *
 * In SVG, the 'overflow' property applies to container elements and determines how to handle
 * content that exceeds the bounds of the viewport.
 */
enum class Overflow {
  Visible,  ///< [DEFAULT] "visible": Content is not clipped, and may render outside the box.
  Hidden,   ///< "hidden": Content is clipped, and no scrollbars are provided.
  Scroll,   ///< "scroll": Content is clipped, but scrollbars are provided to scroll the content.
            ///< Donner does not implement scrolling so this is equivalent to "hidden".
  Auto,     ///< "auto": If content overflows, scrollbars are provided.
};

/**
 * Ostream output operator for \ref Overflow enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, Overflow value) {
  switch (value) {
    case Overflow::Visible: return os << "visible";
    case Overflow::Hidden: return os << "hidden";
    case Overflow::Scroll: return os << "scroll";
    case Overflow::Auto: return os << "auto";
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
