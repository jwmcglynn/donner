#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

/**
 * Parameters for a \ref radialGradient element.
 */
struct RadialGradientComponent {
  std::optional<Lengthd> cx;
  std::optional<Lengthd> cy;
  std::optional<Lengthd> r;
  std::optional<Lengthd> fx;
  std::optional<Lengthd> fy;
  std::optional<Lengthd> fr;

  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

struct ComputedRadialGradientComponent {
  Lengthd cx = Lengthd(50, Lengthd::Unit::Percent);
  Lengthd cy = Lengthd(50, Lengthd::Unit::Percent);
  Lengthd r = Lengthd(50, Lengthd::Unit::Percent);
  // For fx/fy, if they are not specified they will coincide with cx/cy, see
  // https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementFXAttribute. Represent this by
  // using std::nullopt, which will be resolved to cx/cy at the time of rendering.
  std::optional<Lengthd> fx;
  std::optional<Lengthd> fy;
  Lengthd fr = Lengthd(0, Lengthd::Unit::Percent);

  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

}  // namespace donner::svg
