#include "donner/css/Declaration.h"

namespace donner::css {

DeclarationOrAtRule::DeclarationOrAtRule(DeclarationOrAtRule::Type&& value)
    : value(std::move(value)) {}

bool DeclarationOrAtRule::operator==(const DeclarationOrAtRule& other) const {
  return value == other.value;
}

std::ostream& operator<<(std::ostream& os, const Declaration& declaration) {
  os << "  " << declaration.name << ":";
  for (const auto& value : declaration.values) {
    os << " " << value;
  }
  if (declaration.important) {
    os << " !important";
  }
  return os;
}

}  // namespace donner::css
