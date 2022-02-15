#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

// For elements without components, define the presentation attribute template overload for them
// here.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Defs>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::G>(EntityHandle handle,
                                                             std::string_view name,
                                                             const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Unknown>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

}  // namespace donner::svg
