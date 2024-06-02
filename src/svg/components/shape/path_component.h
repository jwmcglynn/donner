#pragma once
/// @file

#include "src/base/rc_string.h"
#include "src/svg/properties/property.h"

namespace donner::svg::components {

struct PathComponent {
  Property<RcString> d{"d", []() -> std::optional<RcString> { return RcString(); }};

  auto allProperties() { return std::forward_as_tuple(d); }
};

}  // namespace donner::svg::components
