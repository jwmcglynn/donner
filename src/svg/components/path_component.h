#pragma once

#include <optional>
#include <string>

#include "src/svg/core/path_spline.h"
#include "src/svg/parser/parse_error.h"

namespace donner {

class PathComponent {
public:
  std::string_view d() const;
  std::optional<ParseError> setD(std::string_view d);

  const std::optional<PathSpline>& spline() const;

private:
  std::string d_;
  std::optional<PathSpline> spline_;
};

}  // namespace donner