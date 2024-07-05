#include "donner/css/selectors/PseudoClassSelector.h"

#include "donner/css/Selector.h"

namespace donner::css {

PseudoClassSelector& PseudoClassSelector::operator=(const PseudoClassSelector& other) {
  ident = other.ident;
  argsIfFunction = other.argsIfFunction;
  anbValueIfAnb = other.anbValueIfAnb;
  selector = other.selector ? std::make_unique<Selector>(*other.selector) : nullptr;

  return *this;
}

std::ostream& operator<<(std::ostream& os, const PseudoClassSelector& obj) {
  os << "PseudoClassSelector(" << obj.ident;
  if (obj.argsIfFunction.has_value()) {
    os << " args[";
    for (auto& arg : obj.argsIfFunction.value()) {
      os << arg << ", ";
    }
    os << "]";
  }
  if (obj.anbValueIfAnb.has_value()) {
    os << " anbValue[" << obj.anbValueIfAnb.value() << "]";
  }
  if (obj.selector) {
    os << " selector[" << obj.selector.get() << "]";
  }
  os << ")";
  return os;
}

}  // namespace donner::css
