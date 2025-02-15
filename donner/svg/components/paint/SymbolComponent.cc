#include "donner/svg/components/paint/SymbolComponent.h"

#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Symbol>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <symbol> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg::parser
