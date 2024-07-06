#pragma once
/// @file

#include <variant>

#include "donner/css/selectors/AttributeSelector.h"
#include "donner/css/selectors/ClassSelector.h"
#include "donner/css/selectors/IdSelector.h"
#include "donner/css/selectors/PseudoClassSelector.h"
#include "donner/css/selectors/PseudoElementSelector.h"
#include "donner/css/selectors/SelectorMatchOptions.h"
#include "donner/css/selectors/TypeSelector.h"

namespace donner::css {

/**
 * A compound selector is a sequence of simple selectors, which represents a set of conditions
 * that are combined to match a single element.
 *
 * For example, the selector `div#foo.bar` is a compound selector, while `div > #foo` is two
 * compound selectors separated by a combinator. Combinators are handled as part of \ref
 * ComplexSelector.
 */
struct CompoundSelector {
  /**
   * A single entry in a compound selector, which can be any of the simple selectors in this
   * variant.
   */
  using Entry = std::variant<PseudoElementSelector, TypeSelector, IdSelector, ClassSelector,
                             PseudoClassSelector, AttributeSelector>;

  /// Default constructor.
  CompoundSelector() = default;

  /// Destructor.
  ~CompoundSelector() noexcept = default;

  /// Moveable and copyable.
  CompoundSelector(const CompoundSelector&) = default;
  CompoundSelector(CompoundSelector&&) = default;
  CompoundSelector& operator=(const CompoundSelector&) = default;
  CompoundSelector& operator=(CompoundSelector&&) = default;

  /// The list of simple selectors in this compound selector.
  std::vector<Entry> entries;

  /**
   * Return true if this selector is valid and supported by this implementation.
   *
   * @see https://www.w3.org/TR/selectors-4/#invalid
   */
  bool isValid() const {
    if (entries.empty()) {
      return false;
    }

    for (const auto& entry : entries) {
      if (!std::visit([](auto&& selector) -> bool { return selector.isValid(); }, entry)) {
        return false;
      }
    }

    return true;
  }

  /**
   * Returns true if the provided element matches this selector.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param element The element to check.
   * @param requirePrimary If true, only primary selectors are considered.
   * @param options Options to control matching.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element, bool requirePrimary,
               const SelectorMatchOptions<T>& options) const {
    for (const auto& entry : entries) {
      if (!std::visit(
              [&element, &options, requirePrimary](auto&& selector) -> bool {
                using SelectorType = std::remove_cvref_t<decltype(selector)>;
                if constexpr (std::is_same_v<SelectorType, PseudoClassSelector>) {
                  const PseudoClassSelector::PseudoMatchResult result =
                      selector.matches(element, options);
                  if (!result.isPrimary && requirePrimary) {
                    return false;
                  }

                  return result.matches;
                } else {
                  return selector.matches(element);
                }
              },
              entry)) {
        return false;
      }
    }

    return !entries.empty();
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const CompoundSelector& obj) {
    os << "CompoundSelector(";
    bool first = true;
    for (auto& entry : obj.entries) {
      if (first) {
        first = false;
      } else {
        os << ", ";
      }

      std::visit([&os](auto&& value) { os << value; }, entry);
    }
    return os << ")";
  }
};

}  // namespace donner::css
