#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/RcString.h"

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

/**
 * Parameters for \ref SVGFEGaussianBlurElement.
 */
struct FEGaussianBlurComponent {
  double stdDeviationX = 0.0;
  double stdDeviationY = 0.0;

  // TODO: in, edgeMode parameters.
};

}  // namespace donner::svg::components
