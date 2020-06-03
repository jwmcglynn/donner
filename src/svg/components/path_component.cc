#include "src/svg/components/path_component.h"

#include "src/svg/parser/path_parser.h"

namespace donner {

std::string_view PathComponent::d() const {
  return d_;
}

std::optional<ParseError> PathComponent::setD(std::string_view d) {
  auto maybePath = PathParser::parse(d);
  if (maybePath.hasResult()) {
    d_ = d;
    spline_ = maybePath.result();
  }

  if (maybePath.hasError()) {
    return std::move(maybePath.error());
  } else {
    return std::nullopt;
  }
}

const std::optional<PathSpline>& PathComponent::spline() const {
  return spline_;
}

}  // namespace donner