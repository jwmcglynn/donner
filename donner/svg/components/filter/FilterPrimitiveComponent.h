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
  std::optional<Lengthd> x;       ///< The x-coordinate of the filter region.
  std::optional<Lengthd> y;       ///< The y-coordinate of the filter region.
  std::optional<Lengthd> width;   ///< The width of the filter region.
  std::optional<Lengthd> height;  ///< The height of the filter region.

  /// Name of the filter primitive, which enables it to be used as a reference for subsequent filter
  /// primitives under the same \ref xml_filter element.
  std::optional<RcString> result;
};

/**
 * Parameters for \ref SVGFEGaussianBlurElement.
 */
struct FEGaussianBlurComponent {
  double stdDeviationX = 0.0;  ///< The standard deviation of the Gaussian blur in the x direction.
  double stdDeviationY = 0.0;  ///< The standard deviation of the Gaussian blur in the y direction.

  // TODO(https://github.com/jwmcglynn/donner/issues/151): in, edgeMode parameters.
};

}  // namespace donner::svg::components
