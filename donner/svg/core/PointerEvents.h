

#pragma once
/// @file

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Values for the 'pointer-events' property, which defines how mouse events interact with an
 * element.
 *
 * @see https://www.w3.org/TR/SVG2/interact.html#PointerEventsProp
 */
enum class PointerEvents {
  None,            //!< Do not respond to pointer events.
  BoundingBox,     //!< The element's bounding box is used instead of the path.
  VisiblePainted,  //!< Responds to pointer events only if the element is visible and the pointer is
                   //!< over the painted area, which is the fill or stroke if they are not `none`.
  VisibleFill,  //!< Responds to pointer events only if the element is visible and within the fill,
                //!< regardless of fill value.
  VisibleStroke,  //!< Responds to pointer events only if the element is visible and within the
                  //!< stroke, regardless of stroke value.
  Visible,  //!< Responds to pointer events only if the element is visible, if the pointer is within
            //!< either the fill or stroke, regardless of their value.
  Painted,  //!< Responds to pointer events only if the pointer is over the painted area, which is
            //!< the fill or stroke if they are not `none`. Ignores the visibility property.
  Fill,  //!< Responds to pointer events only if the pointer is within the fill area, regardless of
         //!< fill or visibility property values.
  Stroke,  //!< Responds to pointer events only if the pointer is within the stroke area, regardless
           //!< of stroke or visibility property values.
  All,  //!< Responds to pointer events regardless of the element's visibility or painted area, if
        //!< the pointer is within either the fill or stroke areas.
};

/// Output stream operator for \ref PointerEvents, outputs the CSS string representation for this
/// enum, e.g. "none", "bounding-box", etc.
inline std::ostream& operator<<(std::ostream& os, PointerEvents value) {
  switch (value) {
    case PointerEvents::None: return os << "none";
    case PointerEvents::BoundingBox: return os << "bounding-box";
    case PointerEvents::VisiblePainted: return os << "visiblePainted";
    case PointerEvents::VisibleFill: return os << "visibleFill";
    case PointerEvents::VisibleStroke: return os << "visibleStroke";
    case PointerEvents::Visible: return os << "visible";
    case PointerEvents::Painted: return os << "painted";
    case PointerEvents::Fill: return os << "fill";
    case PointerEvents::Stroke: return os << "stroke";
    case PointerEvents::All: return os << "all";
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
