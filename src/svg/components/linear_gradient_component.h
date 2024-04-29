#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_linearGradient element.
 */
struct LinearGradientComponent {
  std::optional<Lengthd> x1;
  std::optional<Lengthd> y1;
  std::optional<Lengthd> x2;
  std::optional<Lengthd> y2;

  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

struct ComputedLinearGradientComponent {
  Lengthd x1 = Lengthd(0, Lengthd::Unit::Percent);
  Lengthd y1 = Lengthd(0, Lengthd::Unit::Percent);
  Lengthd x2 = Lengthd(100, Lengthd::Unit::Percent);
  Lengthd y2 = Lengthd(0, Lengthd::Unit::Percent);

  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

}  // namespace donner::svg::components
