#include "donner/css/Selector.h"

namespace donner::css {

//
// ComplexSelector
//

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

Specificity ComplexSelector::computeSpecificity() const {
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
  for (const auto& entry : entries) {
    for (const auto& subEntry : entry.compoundSelector.entries) {
      std::visit(
          [&a, &b, &c](auto&& v) {
            using Type = std::remove_cvref_t<decltype(v)>;

            if constexpr (std::is_same_v<Type, IdSelector>) {
              ++a;
            } else if constexpr (std::is_same_v<Type, ClassSelector> ||
                                 std::is_same_v<Type, AttributeSelector> ||
                                 std::is_same_v<Type, PseudoClassSelector>) {
              // TODO: Handle pseudo-classes that have their specificity defined specially.
              ++b;
            } else if constexpr (std::is_same_v<Type, TypeSelector>) {
              // Ignore the universal selector.
              if (!v.isUniversal()) {
                ++c;
              }
            } else {
              static_assert(std::is_same_v<Type, PseudoElementSelector>);
              ++c;
            }
          },
          subEntry);
    }
  }

  return Specificity::FromABC(a, b, c);
}

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

//
// Selector
//

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
