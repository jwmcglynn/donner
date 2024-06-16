#include "src/svg/components/filter/filter_component.h"  // IWYU pragma: keep

#include "src/svg/properties/presentation_attribute_parsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Filter>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

}  // namespace donner::svg::parser
