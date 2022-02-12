#pragma once

#include "src/svg/components/registry.h"  // For EntityHandle
#include "src/svg/properties/property_parsing.h"

namespace donner::svg {

template <ElementType Type>
ParseResult<bool> ParsePresentationAttribute(EntityHandle handle, std::string_view name,
                                             const PropertyParseFnParams& params);

}  // namespace donner::svg
