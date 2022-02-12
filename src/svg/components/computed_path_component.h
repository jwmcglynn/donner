#pragma once

#include <optional>
#include <string_view>

#include "src/base/parser/parse_error.h"
#include "src/svg/core/path_spline.h"

namespace donner::svg {

class ComputedPathComponent {
public:
  std::optional<PathSpline> spline;
  std::optional<double> userPathLength;
};

}  // namespace donner::svg
