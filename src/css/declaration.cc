#include "src/css/declaration.h"

namespace donner::css {

DeclarationOrAtRule::DeclarationOrAtRule(DeclarationOrAtRule::Type&& value)
    : value(std::move(value)) {}

bool DeclarationOrAtRule::operator==(const DeclarationOrAtRule& other) const {
  return value == other.value;
}

std::ostream& operator<<(std::ostream& os, const Declaration& declaration) {
  os << "Declaration { \n";
  os << "  " << declaration.name << "\n";
  for (const auto& value : declaration.values) {
    os << "  " << value << "\n";
  }
  if (declaration.important) {
    os << "  !important\n";
  }
  return os << "}";
}

}  // namespace donner::css
