#include "donner/svg/components/filter/FilterComponent.h"  // IWYU pragma: keep

#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Filter>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

}  // namespace donner::svg::parser
