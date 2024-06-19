#pragma once
/// @file

#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Holds the value of the `class` attribute of an element.
 */
struct ClassComponent {
  RcString className;  //!< The value of the `class` attribute.
};

}  // namespace donner::svg::components
