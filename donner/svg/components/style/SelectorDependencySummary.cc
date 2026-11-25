#include "donner/svg/components/style/SelectorDependencySummary.h"

#include <algorithm>
#include <array>
#include <variant>

#include "donner/svg/components/style/SelectorWalk.h"

namespace donner::svg::components {
namespace {

using Bits = SelectorDependency::Bits;
using Entry = SelectorDependencySummary::Entry;

constexpr std::uint8_t kSiblingRelations = Bits::FollowingSiblings | Bits::PrecedingSiblings;
constexpr std::uint8_t kVerticalRelations = Bits::Descendants | Bits::Ancestors;

// Opposite directions can reach the edited element again. Keep this conservative even
// when the particular selector would be unable to return along one of the paths.
constexpr std::uint8_t IncludeSelfOnReturnPath(std::uint8_t relations) {
  if ((relations & kSiblingRelations) == kSiblingRelations ||
      (relations & kVerticalRelations) == kVerticalRelations) {
    relations |= Bits::Subject;
  }
  return relations;
}

/// Pseudo-classes whose match depends on an element's position among its siblings or on its
/// children.
bool IsStructuralPseudoClass(const RcString& ident) {
  static constexpr std::array<std::string_view, 11> kStructural = {
      "nth-child",    "nth-last-child", "nth-of-type", "nth-last-of-type",
      "first-child",  "last-child",     "only-child",  "first-of-type",
      "last-of-type", "only-of-type",   "empty",
  };
  return std::any_of(kStructural.begin(), kStructural.end(),
                     [&ident](std::string_view name) { return ident.equalsLowercase(name); });
}

/// Relations a name gains from sitting in a compound left of \p combinator.
std::uint8_t RelationsLeftOf(css::Combinator combinator) {
  return IsSiblingCombinator(combinator) ? Bits::FollowingSiblings : Bits::Descendants;
}

/// Sort \p entries by name with \p compare and merge repeated names, combining their relations.
template <typename Compare>
void SortAndMerge(std::vector<Entry>& entries, Compare compare) {
  std::sort(entries.begin(), entries.end(),
            [&](const Entry& lhs, const Entry& rhs) { return compare(lhs.name, rhs.name) < 0; });
  std::vector<Entry> merged;
  for (Entry& entry : entries) {
    if (!merged.empty() && compare(merged.back().name, entry.name) == 0) {
      merged.back().relations |= entry.relations;
    } else {
      merged.push_back(std::move(entry));
    }
  }
  merged.shrink_to_fit();
  entries = std::move(merged);
}

/// Relations of the entry named \p name in \p entries, sorted by \p compare, or 0.
template <typename Compare>
std::uint8_t Lookup(const std::vector<Entry>& entries, std::string_view name, Compare compare) {
  const auto it = std::lower_bound(
      entries.begin(), entries.end(), name,
      [&](const Entry& entry, std::string_view probe) { return compare(entry.name, probe) < 0; });
  return it != entries.end() && compare(it->name, name) == 0 ? it->relations : 0;
}

}  // namespace

/// Walks selectors into a \ref SelectorDependencySummary.
class SelectorDependencyBuilder {
public:
  explicit SelectorDependencyBuilder(SelectorDependencySummary& summary) : summary_(summary) {}

  /**
   * Record every name of \p selector. \p context holds the relations of the compound the
   * selector is an argument of, or \ref SelectorDependency::Subject at the top level. The
   * selector's subject compounds take \p context; compounds left of a combinator take
   * \p context's non-subject relations plus the combinator's. Recursion is bounded by the CSS
   * parser's nesting limit.
   */
  void addSelector(const css::Selector& selector, std::uint8_t context) {
    for (const css::ComplexSelector& complex : selector.entries) {
      const std::size_t count = complex.entries.size();
      for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t relations =
            i + 1 == count
                ? context
                : static_cast<std::uint8_t>((context & ~Bits::Subject) |
                                            RelationsLeftOf(complex.entries[i + 1].combinator));
        addCompound(complex.entries[i].compoundSelector, IncludeSelfOnReturnPath(relations));
      }
    }
  }

private:
  void addCompound(const css::CompoundSelector& compound, std::uint8_t relations) {
    for (const css::CompoundSelector::Entry& simple : compound.entries) {
      if (const auto* id = std::get_if<css::IdSelector>(&simple)) {
        summary_.ids_.push_back(Entry{id->name, relations});
      } else if (const auto* className = std::get_if<css::ClassSelector>(&simple)) {
        summary_.classes_.push_back(Entry{className->name, relations});
      } else if (const auto* attribute = std::get_if<css::AttributeSelector>(&simple)) {
        addAttribute(attribute->name.name.name, relations);
      } else if (const auto* pseudoClass = std::get_if<css::PseudoClassSelector>(&simple)) {
        addPseudoClass(*pseudoClass, relations);
      }
    }
  }

  void addAttribute(const RcString& localName, std::uint8_t relations) {
    if (localName == "*") {
      summary_.usesAnyAttributeName_ = true;
    } else {
      summary_.attributes_.push_back(Entry{Lowercased(localName), relations});
    }
  }

  /**
   * Record a pseudo-class and the names in its argument. A filtered `:nth-child()` or
   * `:nth-last-child()` whose compound already reaches siblings, because it sits left of a sibling
   * combinator here or in an enclosing selector, reaches siblings in both directions.
   */
  void addPseudoClass(const css::PseudoClassSelector& pseudoClass, std::uint8_t relations) {
    const RcString& ident = pseudoClass.ident;
    summary_.usesStructuralPseudoClass_ |= IsStructuralPseudoClass(ident);
    summary_.dependsOnTextContent_ |= ident.equalsLowercase("empty");
    if (!pseudoClass.selector) {
      return;
    }
    std::uint8_t argument = relations;
    if (ident.equalsLowercase("has")) {
      summary_.usesHas_ = true;
      argument |= Bits::Ancestors | Bits::PrecedingSiblings;
    } else if (ident.equalsLowercase("nth-child")) {
      argument |=
          (relations & kSiblingRelations) != 0 ? kSiblingRelations : Bits::FollowingSiblings;
    } else if (ident.equalsLowercase("nth-last-child")) {
      argument |=
          (relations & kSiblingRelations) != 0 ? kSiblingRelations : Bits::PrecedingSiblings;
    }
    addSelector(*pseudoClass.selector, argument);
  }

  SelectorDependencySummary& summary_;
};

SelectorDependencySummary::SelectorDependencySummary(
    std::span<const css::Stylesheet* const> sheets) {
  SelectorDependencyBuilder builder(*this);
  for (const css::Stylesheet* sheet : sheets) {
    for (const css::SelectorRule& rule : sheet->rules()) {
      builder.addSelector(rule.selector, Bits::Subject);
    }
  }
  SortAndMerge(ids_, CompareExact);
  SortAndMerge(classes_, CompareExact);
  SortAndMerge(attributes_, CompareExact);

  std::size_t keyBytes = 0;
  for (const Entry& entry : attributes_) {
    keyBytes += entry.name.size();
  }
  retainedBytes_ =
      (ids_.capacity() + classes_.capacity() + attributes_.capacity()) * sizeof(Entry) + keyBytes;
}

std::uint8_t SelectorDependencySummary::idDependency(std::string_view id) const {
  return Lookup(ids_, id, CompareExact);
}

std::uint8_t SelectorDependencySummary::classDependency(std::string_view className) const {
  return Lookup(classes_, className, CompareExact);
}

std::uint8_t SelectorDependencySummary::attributeDependency(std::string_view localName) const {
  if (usesAnyAttributeName_) {
    return SelectorDependency::kAll;
  }
  return Lookup(attributes_, localName, CompareLowercased);
}

}  // namespace donner::svg::components
