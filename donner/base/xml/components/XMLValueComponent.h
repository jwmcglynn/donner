#pragma once
/// @file

#include "donner/base/RcString.h"

namespace donner::xml::components {

/**
 * Stores \ref XMLNode values (such as text contents).
 */
struct XMLValueComponent {
  RcString value;  ///< The text/value content of the XML node.
};

}  // namespace donner::xml::components
