#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"

namespace donner::svg {

class SVGDocument;
class SVGElement;

/// Diagnostic record for one stylesheet rule that matched an element.
struct SVGMatchedStyleRule {
  Entity stylesheetEntity = entt::null;  ///< Entity carrying the matched stylesheet.
  std::size_t ruleIndex = 0;             ///< Index of the matched rule within the stylesheet.
  std::size_t selectorEntryIndex = 0;    ///< Index of the matched selector-list entry.
  bool isUserAgentStylesheet = false;    ///< True when this came from the built-in UA stylesheet.
  std::optional<SourceRange> ruleSourceRange;        ///< Matched rule range in SVG source.
  std::optional<SourceRange> selectorSourceRange;    ///< Matched selector branch in SVG source.
  std::vector<SourceRange> declarationSourceRanges;  ///< Declaration ranges in SVG source.

  /// Equality operator.
  bool operator==(const SVGMatchedStyleRule& other) const = default;
};

/// Stylesheet rule found at a document source offset.
struct SVGStyleRuleAtSourceOffset {
  Entity stylesheetEntity = entt::null;           ///< Entity carrying the stylesheet.
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

/// Find the author stylesheet rule at \p documentSourceOffset.
///
/// @param document SVG document to inspect.
/// @param documentSourceOffset Offset in the SVG document source.
/// @return Stylesheet rule at \p documentSourceOffset, or \c std::nullopt.
std::optional<SVGStyleRuleAtSourceOffset> FindStyleRuleAtSourceOffset(
    const SVGDocument& document, std::size_t documentSourceOffset);

}  // namespace donner::svg
