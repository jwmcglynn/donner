#pragma once

#include "src/base/parser/parse_error.h"
#include "src/base/rc_string.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

struct PathComponent {
  Property<RcString> d{"d", []() -> std::optional<RcString> { return RcString(); }};
  std::optional<double> userPathLength;

  auto allProperties() { return std::forward_as_tuple(d); }

  std::optional<ParseError> computePathWithPrecomputedStyle(EntityHandle handle,
                                                            const ComputedStyleComponent& style);

  std::optional<ParseError> computePath(EntityHandle handle);
};

}  // namespace donner::svg
