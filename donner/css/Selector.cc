#include "donner/css/Selector.h"

namespace donner::css {

Selector::Selector() = default;

Selector::~Selector() noexcept = default;

Selector::Selector(Selector&&) noexcept = default;
Selector& Selector::operator=(Selector&&) noexcept = default;
Selector::Selector(const Selector&) = default;
Selector& Selector::operator=(const Selector&) = default;

std::ostream& operator<<(std::ostream& os, const Selector& obj) {
  os << "Selector(";
  bool first = true;
  for (auto& entry : obj.entries) {
    if (first) {
      first = false;
    } else {
      os << ", ";
    }
    os << entry;
  }
  return os << ")";
}

}  // namespace donner::css