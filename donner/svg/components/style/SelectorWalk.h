#pragma once
/// @file
/// Helpers shared by the per-stylesheet selector caches (\ref RuleIndex and
/// \ref SelectorDependencySummary): reading simple selectors out of compounds and comparing the
/// names they key on.

#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/css/selectors/ComplexSelector.h"
#include "donner/css/selectors/CompoundSelector.h"

namespace donner::svg::components {

/**
 * Compare \p stored, which is already ASCII-lowercased, with \p probe ASCII-lowercased, as `<=>`
 * would: negative, zero or positive.
 */
int CompareLowercased(std::string_view stored, std::string_view probe);

/// Compare \p stored with \p probe exactly, as `<=>` would: negative, zero or positive.
int CompareExact(std::string_view stored, std::string_view probe);

/// \p name with ASCII `A`-`Z` lowercased, sharing \p name's storage when it has no capitals.
RcString Lowercased(const RcString& name);

/// The first simple selector of type \p Selector in \p simple that \p accept admits.
template <typename Selector, typename Accept>
const Selector* FindSimple(const std::vector<css::CompoundSelector::Entry>& simple, Accept accept) {
  for (const auto& entry : simple) {
    const auto* selector = std::get_if<Selector>(&entry);
    if (selector != nullptr && accept(*selector)) {
      return selector;
    }
  }
  return nullptr;
}

/// Admits every simple selector of the requested type.
inline constexpr auto kAnySelector = [](const auto& /*selector*/) { return true; };

/// Whether a combinator relates an element to its siblings (`+` or `~`) rather than to its
/// ancestors.
inline bool IsSiblingCombinator(css::Combinator combinator) {
  return combinator == css::Combinator::NextSibling ||
         combinator == css::Combinator::SubsequentSibling;
}

}  // namespace donner::svg::components
