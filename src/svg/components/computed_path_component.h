#pragma once
/// @file

#include <optional>
#include <string_view>

#include "src/base/parser/parse_error.h"
#include "src/svg/core/path_spline.h"

namespace donner::svg::components {

struct ComputedPathComponent {
  PathSpline spline;
};

}  // namespace donner::svg::components
