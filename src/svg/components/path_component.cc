#include "src/svg/components/path_component.h"

#include "src/svg/parser/path_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

std::string_view PathComponent::d() const {
  return d_;
}

void PathComponent::setD(std::string_view d) {
  d_ = d;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Path>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

}  // namespace donner::svg
