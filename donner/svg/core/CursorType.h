#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * CSS cursor types, defining which cursor the user agent should display.
 *
 * @see https://www.w3.org/TR/css-ui-4/#cursor
 */
enum class CursorType : uint8_t {
  Auto,         //!< The browser determines the cursor based on context.
  Default,      //!< Platform default cursor (usually an arrow).
  None,         //!< No cursor is rendered.
  Pointer,      //!< Indicates a link (usually a hand).
  Crosshair,    //!< Cross cursor, often for selection.
  Move,         //!< Indicates something can be moved.
  Text,         //!< Indicates text can be selected.
  Wait,         //!< Indicates the program is busy.
  Help,         //!< Indicates help is available.
  NotAllowed,   //!< Indicates the action is not allowed.
  Grab,         //!< Indicates something can be grabbed.
  Grabbing,     //!< Indicates something is being grabbed.
  // Resize cursors
  NResize,      //!< Resize north.
  EResize,      //!< Resize east.
  SResize,      //!< Resize south.
  WResize,      //!< Resize west.
  NEResize,     //!< Resize north-east.
  NWResize,     //!< Resize north-west.
  SEResize,     //!< Resize south-east.
  SWResize,     //!< Resize south-west.
  ColResize,    //!< Resize column.
  RowResize,    //!< Resize row.
  ZoomIn,       //!< Zoom in.
  ZoomOut,      //!< Zoom out.
};

/// Output stream operator for \ref CursorType, outputs the CSS string representation.
inline std::ostream& operator<<(std::ostream& os, CursorType value) {
  switch (value) {
    case CursorType::Auto: return os << "auto";
    case CursorType::Default: return os << "default";
    case CursorType::None: return os << "none";
    case CursorType::Pointer: return os << "pointer";
    case CursorType::Crosshair: return os << "crosshair";
    case CursorType::Move: return os << "move";
    case CursorType::Text: return os << "text";
    case CursorType::Wait: return os << "wait";
    case CursorType::Help: return os << "help";
    case CursorType::NotAllowed: return os << "not-allowed";
    case CursorType::Grab: return os << "grab";
    case CursorType::Grabbing: return os << "grabbing";
    case CursorType::NResize: return os << "n-resize";
    case CursorType::EResize: return os << "e-resize";
    case CursorType::SResize: return os << "s-resize";
    case CursorType::WResize: return os << "w-resize";
    case CursorType::NEResize: return os << "ne-resize";
    case CursorType::NWResize: return os << "nw-resize";
    case CursorType::SEResize: return os << "se-resize";
    case CursorType::SWResize: return os << "sw-resize";
    case CursorType::ColResize: return os << "col-resize";
    case CursorType::RowResize: return os << "row-resize";
    case CursorType::ZoomIn: return os << "zoom-in";
    case CursorType::ZoomOut: return os << "zoom-out";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
