#pragma once

#include <optional>
#include <string_view>

#include "src/svg/core/path_spline.h"
#include "src/svg/parser/parse_error.h"

namespace donner {

class ComputedPathComponent {
public:
  std::optional<ParseError> setFromDString(std::string_view d);

  void setSpline(std::optional<PathSpline>&& spline);
  const std::optional<PathSpline>& spline() const;

private:
  std::optional<PathSpline> spline_;
};

}  // namespace donner