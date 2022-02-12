#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

// SVGDefsElement has no component, so define the presentation attribute template overload for it
// here.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Defs>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

// SVGUnknownElement has no component, so define the presentation attribute template overload for it
// here.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Unknown>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

}  // namespace donner::svg
