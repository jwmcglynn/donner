#include "donner/css/selectors/ComplexSelector.h"

namespace donner::css {

bool ComplexSelector::isValid() const {
  if (entries.empty()) {
    return false;
  }

  for (const auto& entry : entries) {
    if (!entry.compoundSelector.isValid()) {
      return false;
    }
  }

  return true;
}

Specificity::ABC ComplexSelector::computeSpecificity() const {
  Specificity::ABC result;

  for (const auto& entry : entries) {
    for (const auto& subEntry : entry.compoundSelector.entries) {
      std::visit(
          [&result](auto&& v) {
            using Type = std::remove_cvref_t<decltype(v)>;

            if constexpr (std::is_same_v<Type, IdSelector>) {
              ++result.a;
            } else if constexpr (std::is_same_v<Type, ClassSelector> ||
                                 std::is_same_v<Type, AttributeSelector>) {
              ++result.b;
            } else if constexpr (std::is_same_v<Type, PseudoClassSelector>) {
              const Specificity::ABC pseudoAbc = v.computeSpecificity();

              result.a += pseudoAbc.a;
              result.b += pseudoAbc.b;
              result.c += pseudoAbc.c;
            } else if constexpr (std::is_same_v<Type, TypeSelector>) {
              // Ignore the universal selector.
              if (!v.isUniversal()) {
                ++result.c;
              }
            } else {
              static_assert(std::is_same_v<Type, PseudoElementSelector>);
              ++result.c;
            }
          },
          subEntry);
    }
  }

  return result;
}

/// Ostream output operator for \ref ComplexSelector, outputs debug strings e.g.
/// "ComplexSelector(CompoundSelector(TypeSelector(name)))"
std::ostream& operator<<(std::ostream& os, const ComplexSelector& obj) {
  os << "ComplexSelector(";
  bool first = true;
  for (auto& entry : obj.entries) {
    if (first) {
      first = false;
      os << entry.compoundSelector;
    } else {
      os << " " << entry.combinator << " " << entry.compoundSelector;
    }
  }
  return os << ")";
}

}  // namespace donner::css
