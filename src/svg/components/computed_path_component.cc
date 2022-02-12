#include "src/svg/components/computed_path_component.h"

#include "src/svg/parser/path_parser.h"

namespace donner::svg {

std::optional<ParseError> ComputedPathComponent::setFromDString(std::string_view d) {
  auto maybePath = PathParser::Parse(d);
  if (maybePath.hasResult()) {
    spline_ = std::move(maybePath.result());
  }

  if (maybePath.hasError()) {
    return std::move(maybePath.error());
  } else {
    return std::nullopt;
  }
}

void ComputedPathComponent::setSpline(std::optional<PathSpline>&& spline) {
  spline_ = std::move(spline);
}

const std::optional<PathSpline>& ComputedPathComponent::spline() const {
  return spline_;
}

}  // namespace donner::svg
