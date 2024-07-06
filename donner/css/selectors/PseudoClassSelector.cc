#include "donner/css/selectors/PseudoClassSelector.h"

#include "donner/css/Selector.h"

namespace donner::css {

PseudoClassSelector::PseudoClassSelector(const RcString& ident) : ident(ident) {}

PseudoClassSelector::~PseudoClassSelector() noexcept = default;

PseudoClassSelector::PseudoClassSelector(PseudoClassSelector&&) noexcept = default;
PseudoClassSelector& PseudoClassSelector::operator=(PseudoClassSelector&&) noexcept = default;

PseudoClassSelector::PseudoClassSelector(const PseudoClassSelector& other) {
  this->operator=(other);
}

PseudoClassSelector& PseudoClassSelector::operator=(const PseudoClassSelector& other) {
  ident = other.ident;
  argsIfFunction = other.argsIfFunction;
  anbValueIfAnb = other.anbValueIfAnb;
  selector = other.selector ? std::make_unique<Selector>(*other.selector) : nullptr;

  return *this;
}

Specificity::ABC PseudoClassSelector::computeSpecificity() const {
  // The specificity of an :is(), :not(), or :has() pseudo-class is replaced by the specificity of
  // the most specific complex selector in its selector list argument.
  if (ident.equalsLowercase("is") || ident.equalsLowercase("not") || ident.equalsLowercase("has")) {
    if (selector) {
      return selector->maxSpecificity();
    } else {
      return Specificity::ABC();
    }
  }
  // The specificity of an :nth-child() or :nth-last-child() selector is the specificity of the
  // pseudo class itself (counting as one pseudo-class selector) plus the specificity of the most
  // specific complex selector in its selector list argument (if any).
  else if (ident.equalsLowercase("nth-child") || ident.equalsLowercase("nth-last-child")) {
    if (selector) {
      Specificity::ABC result = selector->maxSpecificity();
      ++result.b;
      return result;
    } else {
      return Specificity::ABC{0, 1, 0};
    }
  }

  // The specificity of a :where() pseudo-class is replaced by zero.
  else if (ident.equalsLowercase("where")) {
    return Specificity::ABC();
  } else {
    // The default specificity of a pseudo-class is b=1.
    return Specificity::ABC{0, 1, 0};
  }
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
