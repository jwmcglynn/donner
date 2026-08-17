#include "donner/css/Selector.h"

#include <algorithm>

namespace donner::css {

namespace {

void CollectAttributeNames(const Selector& selector, std::vector<RcString>& outNames,
                           bool& outMatchesAnyName);

void CollectAttributeNames(const CompoundSelector& compound, std::vector<RcString>& outNames,
                           bool& outMatchesAnyName) {
  for (const CompoundSelector::Entry& entry : compound.entries) {
    if (const auto* attributeSelector = std::get_if<AttributeSelector>(&entry)) {
      const RcString& localName = attributeSelector->name.name.name;
      if (localName == "*") {
        outMatchesAnyName = true;
      } else if (std::find(outNames.begin(), outNames.end(), localName) == outNames.end()) {
        outNames.push_back(localName);
      }
    } else if (const auto* pseudoClassSelector = std::get_if<PseudoClassSelector>(&entry)) {
      if (pseudoClassSelector->selector) {
        CollectAttributeNames(*pseudoClassSelector->selector, outNames, outMatchesAnyName);
      }
    }
  }
}

void CollectAttributeNames(const Selector& selector, std::vector<RcString>& outNames,
                           bool& outMatchesAnyName) {
  for (const ComplexSelector& complexSelector : selector.entries) {
    for (const ComplexSelector::Entry& entry : complexSelector.entries) {
      CollectAttributeNames(entry.compoundSelector, outNames, outMatchesAnyName);
    }
  }
}

}  // namespace

void Selector::collectAttributeSelectorNames(std::vector<RcString>& outNames,
                                             bool& outMatchesAnyName) const {
  CollectAttributeNames(*this, outNames, outMatchesAnyName);
}

Selector::Selector() = default;

Selector::~Selector() noexcept = default;

Selector::Selector(Selector&&) noexcept = default;
Selector& Selector::operator=(Selector&&) noexcept = default;
Selector::Selector(const Selector&) = default;
Selector& Selector::operator=(const Selector&) = default;

/// Ostream output operator for \ref Selector, prints a debug representation of the selector, e.g.
/// `Selector(div, .class, #id)`.
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
