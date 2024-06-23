#include "donner/svg/components/paint/ClipPathComponent.h"

#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::ClipPath>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <clipPath> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg::parser
