#include "src/svg/components/path_component.h"

#include "src/svg/parser/path_parser.h"

namespace donner {

std::string_view PathComponent::d() const {
  return d_;
}

void PathComponent::setD(std::string_view d) {
  d_ = d;
}

}  // namespace donner