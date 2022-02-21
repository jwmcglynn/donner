#pragma once

#include "src/svg/properties/property_parsing.h"
#include "src/svg/registry/registry.h"  // For EntityHandle

namespace donner::svg {

template <ElementType Type>
ParseResult<bool> ParsePresentationAttribute(EntityHandle handle, std::string_view name,
                                             const PropertyParseFnParams& params);

}  // namespace donner::svg
