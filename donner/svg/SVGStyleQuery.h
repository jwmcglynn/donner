#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"
#include "donner/css/Declaration.h"
#include "donner/css/Specificity.h"

namespace donner::svg {

class SVGDocument;
class SVGElement;

/// Diagnostic record for one stylesheet rule that matched an element.
struct SVGMatchedStyleRule {
  Entity stylesheetEntity = entt::null;  ///< Owning `<style>` or `<svg>` element key.
  std::size_t ruleIndex = 0;             ///< Index of the matched rule within the stylesheet.
  std::size_t selectorEntryIndex = 0;    ///< Index of the matched selector-list entry.
  css::Specificity specificity;          ///< Selector specificity for this match.
  bool isUserAgentStylesheet = false;    ///< True when this came from the built-in UA stylesheet.
  std::optional<SourceRange> ruleSourceRange;        ///< Matched rule range in SVG source.
  std::optional<SourceRange> selectorSourceRange;    ///< Matched selector branch in SVG source.
  std::vector<SourceRange> declarationSourceRanges;  ///< Declaration ranges in SVG source.

  /// Equality operator.
  bool operator==(const SVGMatchedStyleRule& other) const = default;
};

/// Parsed declaration within a stylesheet rule, with an optional document source range.
struct SVGStylesheetDeclaration {
  std::size_t declarationIndex = 0;                   ///< Index within the rule.
  css::Declaration declaration;                       ///< Parsed CSS declaration.
  std::optional<SourceRange> declarationSourceRange;  ///< Declaration range in SVG source.
};

/// Stylesheet rule in a document; built-in rules have no document source ranges.
struct SVGStylesheetRule {
  bool isUserAgentStylesheet = false;  ///< True for a built-in rule without document source ranges.
  Entity stylesheetEntity = entt::null;                ///< Owning `<style>` or `<svg>` element key.
  std::size_t ruleIndex = 0;                           ///< Index within the stylesheet.
  std::optional<SourceRange> ruleSourceRange;          ///< Rule range in SVG source.
  std::optional<SourceRange> selectorSourceRange;      ///< Selector range in SVG source.
  std::vector<SVGStylesheetDeclaration> declarations;  ///< Rule declarations.
};

/// Stylesheet rule found at a document source offset.
struct SVGStyleRuleAtSourceOffset {
  Entity stylesheetEntity = entt::null;           ///< Owning `<style>` element key.
  std::size_t ruleIndex = 0;                      ///< Index of the source rule.
  std::optional<std::size_t> selectorEntryIndex;  ///< Selector-list entry containing the offset.
  SourceRange ruleSourceRange{FileOffset::Offset(0),
                              FileOffset::Offset(0)};  ///< Rule range in SVG source.
  SourceRange selectorSourceRange{FileOffset::Offset(0),
                                  FileOffset::Offset(0)};  ///< Selector range in SVG source.

  /// Equality operator.
  bool operator==(const SVGStyleRuleAtSourceOffset& other) const = default;
};

/// Collect stylesheet rules that match \p element.
///
/// @param element Element to inspect.
/// @return Matched stylesheet rules in cascade scan order.
std::vector<SVGMatchedStyleRule> CollectMatchedStyleRules(const SVGElement& element);

/// Collect source-backed author stylesheet rules in \p document, optionally with the built-in user
/// agent stylesheet rules.
///
/// @param document SVG document to inspect.
/// @param includeUserAgent Include built-in rules without document source ranges. Defaults to
/// false.
/// @return Author rules and optionally built-in rules in the existing stylesheet scan order.
std::vector<SVGStylesheetRule> CollectStylesheetRules(const SVGDocument& document,
                                                      bool includeUserAgent = false);

/// Return true when an author stylesheet rule in \p document matches on tree position.
///
/// Such a rule (`#layer rect`, `svg > rect`, `rect:first-child`) can start or stop matching when an
/// element is moved to a different parent or a different place among its siblings, so a caller that
/// must not change how the document paints has to treat a move as unsafe while one exists.
///
/// The user agent stylesheet is excluded. It is a fixed part of every document, and each of its
/// position-based rules keys on the document root (`svg:not(:root)`) or on having a
/// `foreignObject` parent (`*:not(foreignObject) > svg`).
/// Moving an element between a `<g>` and the document root changes none of those, so excluding
/// them does not weaken the answer for a caller asking about such a move.
///
/// @param document SVG document to inspect.
/// @return True when any author stylesheet rule depends on tree position.
bool AuthorStyleDependsOnTreePosition(const SVGDocument& document);

/// Find the author stylesheet rule at \p documentSourceOffset.
///
/// @param document SVG document to inspect.
/// @param documentSourceOffset Offset in the SVG document source.
/// @return Stylesheet rule at \p documentSourceOffset, or \c std::nullopt.
std::optional<SVGStyleRuleAtSourceOffset> FindStyleRuleAtSourceOffset(
    const SVGDocument& document, std::size_t documentSourceOffset);

}  // namespace donner::svg
