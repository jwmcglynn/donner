#pragma once
/// @file

#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/Specificity.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {

/// Diagnostic record for one stylesheet rule that matched an element.
struct MatchedStyleRule {
  Entity stylesheetEntity = entt::null;  ///< Entity carrying the matched stylesheet.
  std::size_t ruleIndex = 0;             ///< Index of the matched rule within the stylesheet.
  std::size_t selectorEntryIndex = 0;    ///< Index of the matched selector-list entry.
  css::Specificity specificity;          ///< Specificity used for cascading this rule.
  bool isUserAgentStylesheet = false;    ///< True when this came from the built-in UA stylesheet.
  std::optional<SourceRange> ruleSourceRange;        ///< Matched rule range in SVG source.
  std::optional<SourceRange> selectorSourceRange;    ///< Matched selector branch in SVG source.
  std::vector<SourceRange> declarationSourceRanges;  ///< Declaration ranges in SVG source.

  /// Equality operator.
  bool operator==(const MatchedStyleRule& other) const = default;
};

/// Stylesheet rule found at a document source offset.
struct StyleRuleAtSourceOffset {
  Entity stylesheetEntity = entt::null;           ///< Entity carrying the stylesheet.
  std::size_t ruleIndex = 0;                      ///< Index of the source rule.
  std::optional<std::size_t> selectorEntryIndex;  ///< Selector-list entry containing the offset.
  SourceRange ruleSourceRange{FileOffset::Offset(0),
                              FileOffset::Offset(0)};  ///< Rule range in SVG source.
  SourceRange selectorSourceRange{FileOffset::Offset(0),
                                  FileOffset::Offset(0)};  ///< Selector range in SVG source.

  /// Equality operator.
  bool operator==(const StyleRuleAtSourceOffset& other) const = default;
};

/**
 * Computes stylesheet information for elements, applying the CSS cascade and inheritance rules.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/shapes.html
 */
class StyleSystem {
public:
  /**
   * Compute the style for the given entity handle, applying the CSS cascade and inheritance rules.
   *
   * @param handle Entity handle to compute the style for
   * @param warningSink Containing any warnings found
   * @returns Computed style component for the entity
   */
  const ComputedStyleComponent& computeStyle(EntityHandle handle, ParseWarningSink& warningSink);

  /**
   * Computes the style for all entities in the registry.
   *
   * @param registry Registry to compute the styles, used to query for all entities in the tree.
   * @param warningSink Containing any warnings found
   */
  void computeAllStyles(Registry& registry, ParseWarningSink& warningSink);

  /**
   * Computes the style for the given entities in the registry.
   *
   * @param registry Registry containing the entities
   * @param entities Entities to compute
   * @param warningSink Containing any warnings found
   */
  void computeStylesFor(Registry& registry, std::span<const Entity> entities,
                        ParseWarningSink& warningSink);

  /**
   * Update the style attribute on an element, merging new declarations with existing ones.
   *
   * Declarations in \p style override existing declarations with the same property name.
   * The merged result is written back to the `style` attribute and the PropertyRegistry is updated.
   *
   * @param handle Entity handle to update.
   * @param style CSS style string to merge, e.g. "fill: red; opacity: 0.5".
   */
  void updateStyle(EntityHandle handle, std::string_view style);

  /**
   * Collect stylesheet rules that match \p handle using the same selector matching and specificity
   * adjustments used by \ref computeStyle.
   *
   * @param handle Entity handle to trace stylesheet matches for.
   * @return Matched stylesheet rules in cascade scan order.
   */
  std::vector<MatchedStyleRule> collectMatchedStyleRules(EntityHandle handle) const;

  /**
   * Find the author stylesheet rule at \p documentSourceOffset.
   *
   * If the offset is inside one selector-list entry, \ref
   * StyleRuleAtSourceOffset::selectorEntryIndex is set so callers can focus that selector branch.
   * If the offset is elsewhere inside the rule, such as a declaration block, the entry is unset and
   * the whole selector list is considered active.
   *
   * @param registry SVG document registry to inspect.
   * @param documentSourceOffset Offset in the SVG document source.
   * @return Stylesheet rule at \p documentSourceOffset, or \c std::nullopt.
   */
  std::optional<StyleRuleAtSourceOffset> findStyleRuleAtSourceOffset(
      Registry& registry, std::size_t documentSourceOffset) const;

  /**
   * Invalidate the computed style for a given entity.
   *
   * @param handle Entity handle to invalidate
   */
  void invalidateComputed(EntityHandle handle);

  /**
   * Invalidate the full style and reparse attributes.
   *
   * @param handle Entity handle to invalidate
   */
  void invalidateAll(EntityHandle handle);

private:
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                             ParseWarningSink& warningSink);
};

}  // namespace donner::svg::components
