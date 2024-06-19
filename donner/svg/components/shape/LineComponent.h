#pragma once
/// @file

#include "donner/base/Length.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_line element.
 *
 * Note that unlike other elements, the `x1`, `y1`, `x2`, and `y2` properties are not presentation
 * attributes, so they must be specified on the element and not through CSS.
 *
 * From https://www.w3.org/TR/SVG2/shapes.html#LineElement
 * > A future specification may convert the ‘x1’, ‘y1’, ‘x2’, and ‘y2’ attributes to geometric
 * > properties. Currently, they can only be specified via element attributes, and not CSS.
 */
struct LineComponent {
  Lengthd x1;
  Lengthd y1;
  Lengthd x2;
  Lengthd y2;
};

}  // namespace donner::svg::components
