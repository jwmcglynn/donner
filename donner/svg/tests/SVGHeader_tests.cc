#include "donner/svg/SVG.h"

namespace donner::svg {

static_assert(requires(const SVGElement& element, std::ostream& output) {
  element.getComputedStyle().opacity.get();
  element.specifiedStyle()->fill.get();
  element.computedStyleIfPresent()->stroke.get();
  output << element.getComputedStyle();
});

}  // namespace donner::svg
