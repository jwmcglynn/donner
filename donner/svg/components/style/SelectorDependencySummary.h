#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/css/Stylesheet.h"

namespace donner::svg::components {

/**
 * Which elements' selector matching can change when an element gains or loses a name (an id, a
 * class, or an attribute), relative to that element. A bit set of \ref Bits values.
 */
struct SelectorDependency {
  /// One relation between the element whose name changed and the elements to restyle.
  enum Bits : std::uint8_t {
    Subject = 1 << 0,            //!< The element itself.
    Descendants = 1 << 1,        //!< Its descendants.
    FollowingSiblings = 1 << 2,  //!< Its following siblings and their descendants.
    PrecedingSiblings = 1 << 3,  //!< Its preceding siblings and their descendants.
    Ancestors = 1 << 4,          //!< Its ancestors and their preceding siblings, through `:has()`.
  };

  /// Every relation: what a wildcard attribute selector implies for any attribute name.
  static constexpr std::uint8_t kAll =
      Subject | Descendants | FollowingSiblings | PrecedingSiblings | Ancestors;
};

/**
 * For every id, class and attribute local name that appears in a stylesheet's selectors, the
 * relations whose selector matching a change to that name can affect, plus the selector features
 * that make a change reach further than names can express.
 *
 * A name in a subject (rightmost) compound affects the element itself; one left of a descendant
 * or child combinator affects descendants; one left of a sibling combinator affects following
 * siblings and their subtrees. A name inside `:is()`, `:where()`, `:not()` or another pseudo-class
 * argument takes the relations of the compound holding the pseudo-class, widened by any
 * combinator inside the argument. A name inside the `of S` filter of `:nth-child()` affects
 * following siblings, and inside that of `:nth-last-child()` preceding siblings, because the
 * filter counts the matching siblings before or after the subject; when the pseudo-class's
 * compound already reaches siblings (left of a sibling combinator, here or in an enclosing
 * selector), both directions. A name inside `:has()` affects ancestors and preceding
 * siblings as well.
 *
 * Built once per parsed stylesheet, from the sheet's own rules and its imports. Names are local:
 * attribute namespaces are ignored, which over-approximates `[ns|name]` to `[name]`.
 */
class SelectorDependencySummary {
public:
  /// A summary of no selectors.
  SelectorDependencySummary() = default;

  /**
   * Summarize every selector of \p sheets.
   *
   * @param sheets Stylesheets whose rules are summarized together.
   */
  explicit SelectorDependencySummary(std::span<const css::Stylesheet* const> sheets);

  /// Relations an id change on an element can affect, compared exactly; 0 if no selector names it.
  std::uint8_t idDependency(std::string_view id) const;

  /// Relations a class change can affect, compared exactly; 0 if no selector names the class.
  std::uint8_t classDependency(std::string_view className) const;

  /**
   * Relations a write of the attribute \p localName can affect, compared ASCII-case-insensitively;
   * every relation when a selector uses a wildcard attribute name.
   */
  std::uint8_t attributeDependency(std::string_view localName) const;

  /// Whether some attribute selector names \p localName, or any name, so a write of that attribute
  /// can change selector matching.
  bool usesAttribute(std::string_view localName) const {
    return attributeDependency(localName) != 0;
  }

  /// Whether a selector uses `:has()`, which makes an element's match depend on its descendants.
  bool usesHas() const { return usesHas_; }

  /// Whether a selector uses a structural pseudo-class (`:nth-*`, `:first-*`, `:last-*`,
  /// `:only-*` or `:empty`).
  bool usesStructuralPseudoClass() const { return usesStructuralPseudoClass_; }

  /// Whether an attribute selector uses a wildcard local name, so any attribute write can change
  /// matching. No CSS syntax produces this today; it is a fail-safe.
  bool usesAnyAttributeName() const { return usesAnyAttributeName_; }

  /// Whether a selector can change matching after a text edit (`:empty`, anywhere).
  bool dependsOnTextContent() const { return dependsOnTextContent_; }

  /// Bytes the summary retains: vector capacities plus the text of lowercased attribute names.
  std::size_t retainedBytes() const { return retainedBytes_; }

  /// One name and the relations a change to it can affect.
  struct Entry {
    RcString name;           //!< The name; lowercased for attributes.
    std::uint8_t relations;  //!< \ref SelectorDependency::Bits.
  };

private:
  friend class SelectorDependencyBuilder;

  std::vector<Entry> ids_;         //!< Sorted by name.
  std::vector<Entry> classes_;     //!< Sorted by name.
  std::vector<Entry> attributes_;  //!< Sorted by lowercased name.
  bool usesHas_ = false;
  bool usesStructuralPseudoClass_ = false;
  bool usesAnyAttributeName_ = false;
  bool dependsOnTextContent_ = false;
  std::size_t retainedBytes_ = 0;
};

}  // namespace donner::svg::components
