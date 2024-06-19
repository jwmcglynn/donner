#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

struct PathComponent {
  Property<RcString> d{"d", []() -> std::optional<RcString> { return RcString(); }};

  auto allProperties() { return std::forward_as_tuple(d); }
};

}  // namespace donner::svg::components
