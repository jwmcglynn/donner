#pragma once
/// @file

#include "src/base/rc_string.h"

namespace donner::svg::components {

/**
 * Holds the value of the `id` attribute of an element.
 */
struct IdComponent {
  RcString id;  //!< The value of the `id` attribute, the element ID.
};

}  // namespace donner::svg::components
