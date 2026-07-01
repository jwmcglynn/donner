#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/components/layout/SizedElementComponent.h"

namespace donner::svg::components {

/**
 * Stores unique parameters for the \ref xml_symbol element.
 *
 * refX and refY provide a reference point (if needed) when the symbol is instantiated by a \ref
 * xml_use element. Per SVG2 they accept a `<length>` (including percentages, which resolve against
 * the symbol's viewBox at layout time) or a keyword that maps to a percentage: refX
 * left/center/right → 0%/50%/100%, refY top/center/bottom → 0%/50%/100%.
 */
struct SymbolComponent {
  Lengthd refX;  //!< The reference x coordinate. Defaults to 0 (no unit).
  Lengthd refY;  //!< The reference y coordinate. Defaults to 0 (no unit).

  /// The properties of the sized element, `x`, `y`, `width`, `height`.
  SizedElementProperties properties;
};

}  // namespace donner::svg::components
