#pragma once

#include "src/base/length.h"

namespace donner::svg {

/**
 * Parameters for a <linearGradient> element.
 */
struct LinearGradientComponent {
  Lengthd x1 = Lengthd(0, Lengthd::Unit::Percent);
  Lengthd y1 = Lengthd(0, Lengthd::Unit::Percent);
  Lengthd x2 = Lengthd(100, Lengthd::Unit::Percent);
  Lengthd y2 = Lengthd(0, Lengthd::Unit::Percent);
};

}  // namespace donner::svg
