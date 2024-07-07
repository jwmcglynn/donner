#pragma once
/// @file

#include <optional>

#include "donner/base/Utils.h"
#include "donner/base/element/ElementLike.h"
#include "donner/css/WqName.h"

namespace donner::css {

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

  /// Destructor.
  ~AttributeSelector() noexcept = default;

  /// Moveable and copyable.
  AttributeSelector(AttributeSelector&&) noexcept = default;
  AttributeSelector& operator=(AttributeSelector&&) noexcept = default;
  AttributeSelector(const AttributeSelector&) noexcept = default;
  AttributeSelector& operator=(const AttributeSelector&) noexcept = default;

  /// Returns true if this is a valid selector.
  bool isValid() const { return true; }

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <ElementLike T>
  bool matches(const T& element) const {
    if (name.name.namespacePrefix != "*") {
      const std::optional<RcString> maybeValue = element.getAttribute(name.name);
      if (!maybeValue) {
        return false;
      }

      // If there's no additional condition, the attribute existing constitutes a match.
      return !matcher || valueMatches(matcher.value(), maybeValue.value());
    } else {
      // Wildcard may return multiple matches.
      const SmallVector<XMLQualifiedNameRef, 1> attributes =
          element.findMatchingAttributes(name.name);

      for (const auto& attributeName : attributes) {
        const std::optional<RcString> maybeValue = element.getAttribute(attributeName);
        assert(maybeValue.has_value() && "Element should exist from findMatchingAttributes");

        // If there's no additional condition, the attribute existing constitutes a match.
        if (!matcher || valueMatches(matcher.value(), maybeValue.value())) {
          return true;
        }
      }

      return false;
    }
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

private:
  static bool valueMatches(const Matcher& m, const RcString& value) {
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
};

}  // namespace donner::css
