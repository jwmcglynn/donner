#include "src/svg/properties/presentation_attribute_parsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Filter>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

}  // namespace donner::svg
