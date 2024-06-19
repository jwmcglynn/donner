#include "donner/css/Rule.h"

namespace donner::css {

AtRule::AtRule(RcString name) : name(std::move(name)) {}

bool AtRule::operator==(const AtRule& other) const {
  return name == other.name && prelude == other.prelude && block == other.block;
}

std::ostream& operator<<(std::ostream& os, const AtRule& rule) {
  os << "AtRule {\n";
  os << "  " << rule.name << "\n";
  for (auto& value : rule.prelude) {
    os << "  " << value << "\n";
  }
  if (rule.block) {
    os << "  { " << *rule.block << " }\n";
  }
  return os << "}";
}

}  // namespace donner::css
