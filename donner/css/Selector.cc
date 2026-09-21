#include "donner/css/Selector.h"

#include <algorithm>
#include <array>

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

bool SelectorDependsOnTreePosition(const Selector& selector);

/// Whether a pseudo-class matches on element state alone, so that moving the element cannot change
/// whether it matches. Any name not listed here is treated as tree-positional, so an unrecognized
/// or newly-specified pseudo-class fails closed.
bool IsElementLocalPseudoClass(const RcString& ident) {
  static constexpr std::array<std::string_view, 12> kElementLocalPseudoClasses = {
      "hover", "focus",  "focus-visible", "active",   "link",    "visited",
      "lang",  "target", "any-link",      "disabled", "enabled", "checked",
  };

  return std::any_of(
      kElementLocalPseudoClasses.begin(), kElementLocalPseudoClasses.end(),
      [&ident](std::string_view candidate) { return ident.equalsLowercase(candidate); });
}

bool PseudoClassDependsOnTreePosition(const PseudoClassSelector& pseudoClass) {
  // `:is()`, `:not()` and `:where()` are as positional as the selectors inside them.
  if (pseudoClass.ident.equalsLowercase("is") || pseudoClass.ident.equalsLowercase("not") ||
      pseudoClass.ident.equalsLowercase("where")) {
    return pseudoClass.selector && SelectorDependsOnTreePosition(*pseudoClass.selector);
  }

  return !IsElementLocalPseudoClass(pseudoClass.ident);
}

bool CompoundDependsOnTreePosition(const CompoundSelector& compound) {
  return std::any_of(
      compound.entries.begin(), compound.entries.end(), [](const CompoundSelector::Entry& entry) {
        const auto* pseudoClass = std::get_if<PseudoClassSelector>(&entry);
        return pseudoClass != nullptr && PseudoClassDependsOnTreePosition(*pseudoClass);
      });
}

bool SelectorDependsOnTreePosition(const Selector& selector) {
  for (const ComplexSelector& complexSelector : selector.entries) {
    if (complexSelector.entries.size() > 1) {
      return true;  // A combinator matches on ancestors or siblings.
    }
    // A relative selector such as `> div` is matched against a reference element, so it is
    // positional even with a single compound. A regular selector list stores Descendant here,
    // where the leading combinator has no effect.
    if (!complexSelector.entries.empty() &&
        complexSelector.entries.front().combinator != Combinator::Descendant) {
      return true;
    }

    for (const ComplexSelector::Entry& entry : complexSelector.entries) {
      if (CompoundDependsOnTreePosition(entry.compoundSelector)) {
        return true;
      }
    }
  }

  return false;
}

}  // namespace

void Selector::collectAttributeSelectorNames(std::vector<RcString>& outNames,
                                             bool& outMatchesAnyName) const {
  CollectAttributeNames(*this, outNames, outMatchesAnyName);
}

bool Selector::dependsOnTreePosition() const {
  return SelectorDependsOnTreePosition(*this);
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
