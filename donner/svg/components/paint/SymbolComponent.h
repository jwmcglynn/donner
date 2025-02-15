#pragma once
/// @file

#include "donner/svg/components/layout/SizedElementComponent.h"

namespace donner::svg::components {

/**
 * Stores unique parameters for the \ref xml_symbol element.
 *
 * refX and refY provide a reference point (if needed) when the symbol is instantiated by a \ref
 * xml_use element.
 */
struct SymbolComponent {
  double refX = 0.0;  //!< The reference x coordinate.
  double refY = 0.0;  //!< The reference y coordinate.

  /// The properties of the sized element, `x`, `y`, `width`, `height`.
  SizedElementProperties properties;
};

}  // namespace donner::svg::components
