#pragma once
/// @file

#include <cassert>
#include <concepts>
#include <coroutine>
#include <functional>
#include <variant>
#include <vector>

#include "src/base/utils.h"
#include "src/css/component_value.h"
#include "src/css/details/anb_value.h"
#include "src/css/selector_traversal.h"
#include "src/css/specificity.h"
#include "src/svg/xml/xml_attribute.h"

namespace donner::css {

/**
 * Returned by \ref Selector::matches to indicate whether the selector matched, and if so, the
 * specificity of the match.
 *
 * The `bool` operator can be used to check if the selector matched:
 * ```
 * if (auto match = selector.matches(element); match) {
 *   // ...
 * }
 * ```
 *
 * To construct, use the static methods: \ref None() and \ref Match().
 */
struct SelectorMatchResult {
  bool matched = false;     ///< True if the selector matched.
  Specificity specificity;  ///< The specificity of the match, if matched.

  /**
   * Create a SelectorMatchResult indicating that the selector did not match.
   */
  static constexpr SelectorMatchResult None() { return SelectorMatchResult(); }

  /**
   * Create a SelectorMatchResult indicating that the selector matched, with the given specificity.
   *
   * @param specificity The specificity of the match.
   */
  static constexpr SelectorMatchResult Match(Specificity specificity) {
    return SelectorMatchResult(true, specificity);
  }

  /// Returns true if the selector matched.
  explicit operator bool() const { return matched; }

private:
  // Use the static methods above to construct.
  constexpr SelectorMatchResult() = default;

  constexpr SelectorMatchResult(bool matched, Specificity specificity)
      : matched(matched), specificity(specificity) {}
};

/**
 * A CSS qualified name, which is a name optionally associated with a namespace. See
 * https://www.w3.org/TR/selectors-4/#type-nmsp for the full definition.
 *
 * For example, the following are all valid qualified names:
 * - `foo`, represents the name `foo` which belongs to the default namespace.
 * - `|foo`, represents the name `foo` which belongs to no namespace.
 * - `ns|foo`, represents the name `foo` which belongs to the namespace `ns`.
 * - `*|foo`, represents the name `foo` which belongs to any namespace.
 */
struct WqName {
  svg::XMLAttribute name;

  /**
   * Create a WqName with the given namespace and name.
   *
   * @param ns The namespace of the name, or empty if the name belongs to the default namespace.
   * @param name The name.
   */
  WqName(svg::XMLAttribute&& name) : name(std::move(name)) {}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const WqName& obj) { return os << obj.name; }
};

/**
 * Selectors which start with two colons are called pseudo-elements, e.g. '::before'. They are used
 * to represent elements which are not directly present in the document tree.
 *
 * See https://www.w3.org/TR/selectors-4/#pseudo-elements for the full definition.
 *
 * Pseudo-elements are listed in the CSS Pseudo-Elements Module Level 4 spec:
 * https://www.w3.org/TR/css-pseudo-4/
 */
struct PseudoElementSelector {
  RcString ident;  ///< The identifier of the pseudo-element.
  std::optional<std::vector<ComponentValue>>
      argsIfFunction;  ///< The arguments to the function, if this is a function.

  /**
   * Create a PseudoElementSelector with the given identifier.
   *
   * @param ident The identifier of the pseudo-element.
   */
  explicit PseudoElementSelector(RcString ident) : ident(std::move(ident)) {}

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    // TODO
    return false;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const PseudoElementSelector& obj) {
    os << "PseudoElementSelector(" << obj.ident;
    if (obj.argsIfFunction.has_value()) {
      os << " args[";
      for (auto& arg : obj.argsIfFunction.value()) {
        os << arg << ", ";
      }
      os << "]";
    }
    os << ")";
    return os;
  }
};

/**
 * Selector which matches the element type, e.g. `div` or `circle`.  The selector may also include a
 * namespace, or be a universal selector.
 *
 * In the CSS source, this is represented by either a standalone type, or namespace and type
 * separated by a pipe (`|`). Either the namespace or the type may be a wildcard (`*`).
 * - `type`
 * - `*`
 * - `ns|type`
 * - `ns|*`
 * - `*|type`
 *
 * TypeSelector represents the parsed representation, and if the namespace is empty the \ref ns
 * value is empty.
 */
struct TypeSelector {
  /**
   * Selector itself, which may contain wildcards.
   *
   * In this context, the members have the following meanings:
   * - \ref XMLAttribute::namespacePrefix The namespace of the selector, the wildcard namespace
   ("*"), or empty if no namespace is specified.
   * - \ref XMLAttribute::name The name of the selector, or "*" if the selector is a universal
   selector.
   */
  svg::XMLAttribute name;

  /**
   * Create a TypeSelector with the given namespace and name.
   *
   * @param ns The namespace of the selector, the wildcard namespace ("*"), or empty if no namespace
   *   is specified.
   * @param name The name of the selector, or "*" if the selector is a universal selector.
   */
  TypeSelector(svg::XMLAttribute&& name) : name(std::move(name)) {}

  /// Returns true if this is a universal selector.
  bool isUniversal() const { return name.name == "*"; }

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    // TODO: Also check the namespace.
    if (UTILS_PREDICT_FALSE(isUniversal())) {
      return true;
    } else {
      return element.typeString().equalsIgnoreCase(name.name);
    }
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const TypeSelector& obj) {
    return os << "TypeSelector(" << obj.name << ")";
  }
};

/**
 * Selector which match the element's `id` attribute, for example `#foo` matches an element with
 * an `id="foo"` attribute.
 *
 * See https://www.w3.org/TR/selectors-4/#id-selectors for the full definition.
 */
struct IdSelector {
  RcString name;  ///< The id to match, without the leading `#`.

  /**
   * Create an IdSelector with the given name.
   *
   * @param name The id to match, without the leading `#`.
   */
  explicit IdSelector(RcString name) : name(std::move(name)) {}

  /**
   * Returns true if the provided element matches this selector, based on a case-sensitive match
   * of the provided id against the element's `id` attribute.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    return element.id() == name;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const IdSelector& obj) {
    os << "IdSelector(" << obj.name << ")";
    return os;
  }
};

/**
 * Selector which match the element's `class` attribute, for example `.foo` matches an element with
 * class `foo`.
 *
 * See https://www.w3.org/TR/selectors-4/#class-html for the full definition.
 */
struct ClassSelector {
  RcString name;  ///< The class to match, without the leading `.`.

  /**
   * Create a ClassSelector with the given name.
   *
   * @param name The class to match, without the leading `.`.
   */
  explicit ClassSelector(RcString name) : name(std::move(name)) {}

  /**
   * Returns true if the provided element matches this selector, based on if the element's `class`
   * attribute's whitespace-separated list of classes exactly contains this selector's name.
   *
   * Comparison is case-sensitive.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    // Matching is equivalent to `[class=~name]`.

    // Returns true if attribute value is a whitespace-separated list of values, and one of them
    // exactly matches the matcher value.
    for (auto&& str : StringUtils::Split(element.className(), ' ')) {
      if (str == name) {
        return true;
      }
    }

    return false;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const ClassSelector& obj) {
    os << "ClassSelector(" << obj.name << ")";
    return os;
  }
};

/**
 * An+B microsyntax value with an optional selector, for pseudo-class selectors such as
 *`:nth-child(An+B of S)`.
 */
struct AnbValueAndSelector {
  AnbValue value;                        //!< The An+B value.
  std::optional<TypeSelector> selector;  //!< The optional selector.
};

/**
 * Selectors which start with one colon, e.g. `:hover`, are called pseudo-classes, and they
 * represent additional state information not directly present in the document tree.
 *
 * Each pseudo-class selector has a unique behavior.
 *
 * Pseudo-classes are defined in the following specs:
 * - Linguistic Pseudo-classes, such as `:dir()` and `:lang()`,
 *   https://www.w3.org/TR/selectors-4/#linguistic-pseudos
 * - Location Pseudo-classes, such as `:link` and `:visited`,
 *   https://www.w3.org/TR/selectors-4/#location
 * - User Action Pseudo-classes, such as `:hover` and `:active`,
 *   https://www.w3.org/TR/selectors-4/#useraction-pseudos
 * - Time-dimensional Pseudo-classes, such as `:current` and `:past`,
 *   https://www.w3.org/TR/selectors-4/#time-pseudos
 * - Resource State Pseudo-classes, such as `:playing` and `:muted`,
 *   https://www.w3.org/TR/selectors-4/#resource-pseudos
 * - Element Display State Pseudo-classes, such as `:open` and `:fullscreen`,
 *   https://www.w3.org/TR/selectors-4/#display-state-pseudos
 * - Input Pseudo-classes, such as `:enabled` and `:checked`,
 *   https://www.w3.org/TR/selectors-4/#input-pseudos
 * - Tree-Structural Pseudo-classes, such as `:empty` and `:nth-child()`,
 *   https://www.w3.org/TR/selectors-4/#structural-pseudos
 */
struct PseudoClassSelector {
  RcString ident;  ///< The name of the pseudo-class.
  std::optional<std::vector<ComponentValue>>
      argsIfFunction;  ///< The arguments of the pseudo-class, if it is a function.
  std::optional<AnbValueAndSelector>
      anbValueAndSelectorIfAnb;  ///< The An+B value and selector of the pseudo-class, for An+B
                                 ///< pseudo-classes such as `:nth-child`.

  /**
   * Create a PseudoClassSelector with the given ident.
   *
   * @param ident The name of the pseudo-class.
   */
  explicit PseudoClassSelector(RcString ident) : ident(std::move(ident)) {}

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    if (!argsIfFunction.has_value()) {
      if (ident.equalsLowercase("root")) {
        return !element.parentElement().has_value();
      } else if (ident.equalsLowercase("empty")) {
        return !element.firstChild().has_value();
      } else if (ident.equalsLowercase("first-child")) {
        return !element.previousSibling().has_value();
      } else if (ident.equalsLowercase("last-child")) {
        return !element.nextSibling().has_value();
      } else if (ident.equalsLowercase("only-child")) {
        return !element.previousSibling().has_value() && !element.nextSibling().has_value();
      }
    } else {
      // It's a function.

      const std::optional<T> maybeParent = element.parentElement();
      if (!maybeParent) {
        return false;
      }

      if (ident.equalsLowercase("nth-child") && anbValueAndSelectorIfAnb) {
        const int childIndex = getIndexInParent(*maybeParent, element, /*fromEnd*/ false,
                                                anbValueAndSelectorIfAnb->selector);
        return anbValueAndSelectorIfAnb->value.evaluate(childIndex);
      } else if (ident.equalsLowercase("nth-last-child") && anbValueAndSelectorIfAnb) {
        const int childIndex = getIndexInParent(*maybeParent, element, /*fromEnd*/ true,
                                                anbValueAndSelectorIfAnb->selector);
        return anbValueAndSelectorIfAnb->value.evaluate(childIndex);
      }
    }

    return false;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const PseudoClassSelector& obj) {
    os << "PseudoClassSelector(" << obj.ident;
    if (obj.argsIfFunction.has_value()) {
      os << " args[";
      for (auto& arg : obj.argsIfFunction.value()) {
        os << arg << ", ";
      }
      os << "]";
    }
    if (obj.anbValueAndSelectorIfAnb.has_value()) {
      os << " anbValue[" << obj.anbValueAndSelectorIfAnb.value().value;
      if (obj.anbValueAndSelectorIfAnb->selector.has_value()) {
        os << " of " << obj.anbValueAndSelectorIfAnb->selector.value();
      }
      os << "]";
    }
    os << ")";
    return os;
  }

private:
  template <traversal::ElementLike T>
  static int getIndexInParent(const T& parent, const T& element, bool fromEnd,
                              std::optional<TypeSelector> matchingType) {
    int childIndex = 1;
    if (!fromEnd) {
      for (std::optional<T> child = parent.firstChild(); child;
           child = child.value().nextSibling()) {
        if (matchingType && !matchingType->matches(child.value())) {
          continue;
        }

        if (child.value() == element) {
          return childIndex;
        } else {
          ++childIndex;
        }
      }
    } else {
      for (std::optional<T> child = parent.lastChild(); child;
           child = child.value().previousSibling()) {
        if (matchingType && !matchingType->matches(child.value())) {
          continue;
        }

        if (child.value() == element) {
          return childIndex;
        } else {
          ++childIndex;
        }
      }
    }

    assert(matchingType &&
           "Should only reach end of child list if there is a TypeSelector skipping elements");
    return -1;
  }
};

/**
 * For attribute selectors, different match modes are available, which are specified by this enum.
 *
 * See https://www.w3.org/TR/selectors-4/#attribute-selectors for the full definition.
 *
 * These are used within square brackets on the selector list, such as `a[href^="https://"]` or
 * `h1[title]`, and AttrMatcher represents the separator between the attribute name and string,
 * such as `^=` or `=`.
 */
enum class AttrMatcher {
  Includes,     ///< "~=", matches if the attribute value is a whitespace-separated list of values,
                ///< and one of them exactly matches the matcher value.
  DashMatch,    ///< "|=", matches if the attribute value either exactly matches, or begins with the
                ///< value immediately followed by a dash ("-").
  PrefixMatch,  ///< "^=", matches if the attribute value begins with the matcher value.
  SuffixMatch,  ///< "$=", matches if the attribute value ends with the matcher value.
  SubstringMatch,  ///< "*=", matches if the attribute value contains the matcher value.
  Eq,              ///< "=", matches if the attribute value exactly matches the matcher value.
};

/// Ostream output operator.
inline std::ostream& operator<<(std::ostream& os, AttrMatcher matcher) {
  switch (matcher) {
    case AttrMatcher::Includes: return os << "Includes(~=)";
    case AttrMatcher::DashMatch: return os << "DashMatch(|=)";
    case AttrMatcher::PrefixMatch: return os << "PrefixMatch(^=)";
    case AttrMatcher::SuffixMatch: return os << "SuffixMatch($=)";
    case AttrMatcher::SubstringMatch: return os << "SubstringMatch(*=)";
    case AttrMatcher::Eq: return os << "Eq(=)";
  }

  UTILS_UNREACHABLE();
}

/**
 * Selectors which match against element attributes, such as `a[href^="https://"]` or `h1[title]`.
 *
 * See https://www.w3.org/TR/selectors-4/#attribute-selectors for the full definition.
 *
 * Attribute selectors start with a square bracket, specify an attribute name, and an optional
 * Matcher condition to allow matching against the attribute contents.
 */
struct AttributeSelector {
  /**
   * Matcher condition for an attribute selector.
   *
   * This is set when the selector includes a match operator, such as `^=` or `=`, and includes a
   * string and an optional case-insensitive flag.
   *
   * For a standard case-sensitive matcher, this appears in the source as:
   * ```
   * [attr="value"]
   * ```
   *
   * For a case-insensitive matcher, an "i" suffix is added:
   * ```
   * [attr="value" i]
   * ```
   */
  struct Matcher {
    AttrMatcher op;                ///< The match operator.
    RcString value;                ///< The value to match against.
    bool caseInsensitive = false;  ///< Whether to match case-insensitively.
  };

  WqName name;                     ///< Attribute name.
  std::optional<Matcher> matcher;  ///< Optional matcher condition. If this is not specified, the
                                   ///< attribute existing is sufficient for a match.

  /**
   * Create an AttributeSelector with the given name.
   *
   * @param name The attribute name.
   */
  explicit AttributeSelector(WqName name) : name(std::move(name)) {}

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    const std::optional<RcString> maybeValue = element.getAttribute(name.name);
    if (!maybeValue) {
      return false;
    }

    // If there's no additional condition, the attribute existing constitutes a match.
    if (!matcher) {
      return true;
    }

    const Matcher& m = matcher.value();
    const RcString& value = maybeValue.value();

    switch (m.op) {
      case AttrMatcher::Includes:
        // Returns true if attribute value is a whitespace-separated list of values, and one of
        // them exactly matches the matcher value.
        for (auto&& str : StringUtils::Split(value, ' ')) {
          if (m.caseInsensitive) {
            if (StringUtils::Equals<StringComparison::IgnoreCase>(str, m.value)) {
              return true;
            }
          } else {
            if (str == m.value) {
              return true;
            }
          }
        }
        break;
      case AttrMatcher::DashMatch:
        // Matches if the attribute exactly matches, or matches the start of the value plus a
        // hyphen. For example, "foo" matches "foo" and "foo-bar", but not "foobar".
        if (m.caseInsensitive) {
          return value.equalsIgnoreCase(m.value) ||
                 StringUtils::StartsWith<StringComparison::IgnoreCase>(value, m.value + "-");
        } else {
          return value == m.value || StringUtils::StartsWith(value, m.value + "-");
        }
        break;
      case AttrMatcher::PrefixMatch:
        if (m.caseInsensitive) {
          return StringUtils::StartsWith<StringComparison::IgnoreCase>(value, m.value);
        } else {
          return StringUtils::StartsWith(value, m.value);
        }
        break;
      case AttrMatcher::SuffixMatch:
        if (m.caseInsensitive) {
          return StringUtils::EndsWith<StringComparison::IgnoreCase>(value, m.value);
        } else {
          return StringUtils::EndsWith(value, m.value);
        }
        break;
      case AttrMatcher::SubstringMatch:
        if (m.caseInsensitive) {
          return StringUtils::Contains<StringComparison::IgnoreCase>(value, m.value);
        } else {
          return StringUtils::Contains(value, m.value);
        }
        break;
      case AttrMatcher::Eq:
        if (m.caseInsensitive) {
          return value.equalsIgnoreCase(m.value);
        } else {
          return value == m.value;
        }
    }

    return false;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const AttributeSelector& obj) {
    os << "AttributeSelector(" << obj.name;
    if (obj.matcher) {
      os << " " << obj.matcher->op << " " << obj.matcher->value;
      if (obj.matcher->caseInsensitive) {
        os << " (case-insensitive)";
      }
    }
    os << ")";
    return os;
  }
};

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

  /// The list of simple selectors in this compound selector.
  std::vector<Entry> entries;

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    for (const auto& entry : entries) {
      if (!std::visit([&element](auto&& selector) -> bool { return selector.matches(element); },
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

/**
 * Between two compound selectors, there can be a combinator, which specifies how the two elements
 * are associated in the tree.
 *
 * By default, a space between compound selectors is a descendant combinator, e.g. `div span` is a
 * \ref Descendant combinator, while `div > span` is a \ref Child combinator.
 */
enum class Combinator {
  Descendant,         ///< Space-separated, finds descendants in the tree.
  Child,              ///< '>', finds direct children in the tree.
  NextSibling,        ///< '+', finds the next sibling in the tree.
  SubsequentSibling,  ///< '~', finds all subsequent siblings in the tree.
  Column,  ///< '||', finds the next column in the tree. Note that this is a new feature in CSS
           ///< Selectors Level 4, however at the version of the spec this was written against,
           ///< https://www.w3.org/TR/2018/WD-selectors-4-20181121/, it was at-risk of being
           ///< removed.
};

/**
 * Ostream output operator. Outputs the combinator character, e.g. ' ', '>', '+', '~' or '||'.
 *
 * @param os The output stream.
 * @param combinator The combinator.
 */
inline std::ostream& operator<<(std::ostream& os, Combinator combinator) {
  switch (combinator) {
    case Combinator::Descendant: return os << "' '";
    case Combinator::Child: return os << "'>'";
    case Combinator::NextSibling: return os << "'+'";
    case Combinator::SubsequentSibling: return os << "'~'";
    case Combinator::Column: return os << "'||'";
  }
}

/**
 * A complex selector is a sequence of one or more compound selectors, separated by combinators.
 *
 * For example, `div > #foo` is a complex selector, with two compound selectors separated by a
 * \ref Combinator::Child.
 */
struct ComplexSelector {
  /// A single entry in a complex selector, which is a compound selector and a combinator.
  struct Entry {
    Combinator combinator;  ///< The combinator between this compound selector and the next. For
                            ///< the first Entry, this is set to \ref Combinator::Descendant but
                            ///< it has no effect.
    CompoundSelector compoundSelector;  ///< The compound selector.
  };

  std::vector<Entry> entries;  ///< The entries in the complex selector.

  /**
   * Compute specificity of the ComplexSelector, see
   * https://www.w3.org/TR/selectors-4/#specificity-rules.
   */
  Specificity computeSpecificity() const {
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

  /**
   * Match a selector against an element, following the rules in the spec:
   * https://www.w3.org/TR/selectors-4/#match-against-element
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param targetElement Element to match against.
   * @return true if the element matches the selector, within a SelectorMatchResult which also
   *              contains the specificity.
   */
  template <traversal::ElementLike T>
  SelectorMatchResult matches(const T& targetElement) const {
    // TODO: Accept :scope elements.
    using GeneratorCreator = std::function<traversal::SelectorTraversalGenerator<T>()>;
    GeneratorCreator elementsGenerator =
        std::bind(&traversal::singleElementGenerator<T>, targetElement);

    // "To match a complex selector against an element, process it compound selector at a time, in
    // right-to-left order."
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      const auto& entry = *it;

      // "If any simple selectors in the rightmost compound selector does not match the element,
      // return failure."
      std::optional<T> currentElement;
      traversal::SelectorTraversalGenerator<T> elements = elementsGenerator();
      while (elements.next()) {
        const T element = elements.getValue();
        if (entry.compoundSelector.matches(element)) {
          currentElement = element;
          break;
        }
      }

      if (!currentElement) {
        return SelectorMatchResult::None();
      }

      // "Otherwise, if there is only one compound selector in the complex selector, return
      // success."
      // In this case, return success once we've reached the leftmost compound selector.
      if (it + 1 == entries.rend()) {
        return SelectorMatchResult::Match(computeSpecificity());
      }

      // "Otherwise, consider all possible elements that could be related to this element by the
      // rightmost combinator. If the operation of matching the selector consisting of this
      // selector with the rightmost compound selector and rightmost combinator removed against
      // any one of these elements returns success, then return success. Otherwise, return
      // failure."
      if (entry.combinator == Combinator::Descendant) {
        elementsGenerator = std::bind(&traversal::parentsGenerator<T>, currentElement.value());
      } else if (entry.combinator == Combinator::Child) {
        elementsGenerator =
            std::bind(&traversal::singleElementGenerator<T>, currentElement->parentElement());
      } else if (entry.combinator == Combinator::NextSibling) {
        elementsGenerator =
            std::bind(&traversal::singleElementGenerator<T>, currentElement->previousSibling());
      } else if (entry.combinator == Combinator::SubsequentSibling) {
        elementsGenerator =
            std::bind(&traversal::previousSiblingsGenerator<T>, currentElement.value());
      } else {
        // TODO: Combinator::Column
        return SelectorMatchResult::None();
      }
    }

    // Default to not match.
    return SelectorMatchResult::None();
  }

  /// Output a human-readable representation of the selector.
  friend std::ostream& operator<<(std::ostream& os, const ComplexSelector& obj) {
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
};

/**
 * A top-level Selector, which is a list of \ref ComplexSelector.
 *
 * This represents the prelude in front of any CSS rule, e.g. `div.foo > span#bar`, which would be
 * a single \ref ComplexSelector. For a comma-separated list, such as `div.foo > span#bar,
 * span#bar`, this would be a \ref Selector with two \ref ComplexSelector entries.
 */
struct Selector {
  /// The list of \ref ComplexSelector entries that compose this selector.
  std::vector<ComplexSelector> entries;

  /**
   * Match an element against a Selector.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param targetElement Element to match against.
   * @returns true if any ComplexSelector in the Selector matches the given element.
   */
  template <traversal::ElementLike T>
  SelectorMatchResult matches(const T& targetElement) const {
    for (const auto& entry : entries) {
      if (auto result = entry.matches(targetElement)) {
        return result;
      }
    }

    return SelectorMatchResult::None();
  }

  /**
   * Ostream output operator for Selector in a human-readable format.
   */
  friend std::ostream& operator<<(std::ostream& os, const Selector& obj) {
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
};

}  // namespace donner::css
