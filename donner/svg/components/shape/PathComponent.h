#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

struct PathComponent {
  Property<RcString> d{"d", []() -> std::optional<RcString> { return RcString(); }};

  std::optional<PathSpline> splineOverride;

  auto allProperties() { return std::forward_as_tuple(d); }
};

}  // namespace donner::svg::components
