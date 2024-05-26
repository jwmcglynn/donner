#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/base/rc_string.h"

namespace donner::svg::components {

/**
 * Parameters for \ref SVGFilterPrimitiveStandardAttributes.
 */
struct FilterPrimitiveComponent {
  std::optional<Lengthd> x;
  std::optional<Lengthd> y;
  std::optional<Lengthd> width;
  std::optional<Lengthd> height;
  std::optional<RcString> result;
};

}  // namespace donner::svg::components
