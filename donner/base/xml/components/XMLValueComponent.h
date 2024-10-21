#pragma once
/// @file

#include "donner/base/RcString.h"

namespace donner::xml::components {

/**
 * Stores \ref XMLNode values (such as text contents).
 */
struct XMLValueComponent {
  RcString value;
};

}  // namespace donner::xml::components
