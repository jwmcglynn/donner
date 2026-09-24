#include "donner/svg/SVGElement.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg {

static_assert(requires(const SVGElement& element, std::ostream& output) {
  element.getComputedStyle().opacity.get();
  element.specifiedStyle()->fill.get();
  element.computedStyleIfPresent()->stroke.get();
  output << element.getComputedStyle();
});

}  // namespace donner::svg
