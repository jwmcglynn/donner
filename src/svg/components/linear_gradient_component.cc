#include "src/svg/components/linear_gradient_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::LinearGradient>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <radialGradient> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg
