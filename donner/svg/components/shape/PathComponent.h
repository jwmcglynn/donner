#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/base/Path.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_path element.
 */
struct PathComponent {
  /// The path data string, defaults to an empty string.
  Property<RcString> d{"d", []() -> std::optional<RcString> { return RcString(); }};

  /// Overridden path spline, if the user has provided a pre-parsed spline through \ref
  /// SVGPathElement::setSpline. If set, \ref d will not be used.
  std::optional<Path> splineOverride;

  /// Get all properties as a tuple.
  auto allProperties() { return std::forward_as_tuple(d); }
};

}  // namespace donner::svg::components
