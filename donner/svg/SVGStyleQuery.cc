#include "donner/svg/SVGStyleQuery.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg {
namespace {

SVGMatchedStyleRule ToPublicMatchedRule(const components::MatchedStyleRule& rule) {
  return SVGMatchedStyleRule{
      .stylesheetEntity = rule.stylesheetEntity,
      .ruleIndex = rule.ruleIndex,
      .selectorEntryIndex = rule.selectorEntryIndex,
      .isUserAgentStylesheet = rule.isUserAgentStylesheet,
      .ruleSourceRange = rule.ruleSourceRange,
      .selectorSourceRange = rule.selectorSourceRange,
      .declarationSourceRanges = rule.declarationSourceRanges,
  };
}

SVGStyleRuleAtSourceOffset ToPublicSourceRule(const components::StyleRuleAtSourceOffset& rule) {
  return SVGStyleRuleAtSourceOffset{
      .stylesheetEntity = rule.stylesheetEntity,
      .ruleIndex = rule.ruleIndex,
      .selectorEntryIndex = rule.selectorEntryIndex,
      .ruleSourceRange = rule.ruleSourceRange,
      .selectorSourceRange = rule.selectorSourceRange,
  };
}

}  // namespace

std::vector<SVGMatchedStyleRule> CollectMatchedStyleRules(const SVGElement& element) {
  if (!element.entityHandle()) {
    return {};
  }

  std::vector<components::MatchedStyleRule> internalRules =
      components::StyleSystem().collectMatchedStyleRules(element.entityHandle());
  std::vector<SVGMatchedStyleRule> rules;
  rules.reserve(internalRules.size());
  for (const components::MatchedStyleRule& rule : internalRules) {
    rules.push_back(ToPublicMatchedRule(rule));
  }
  return rules;
}

std::optional<SVGStyleRuleAtSourceOffset> FindStyleRuleAtSourceOffset(
    const SVGDocument& document, std::size_t documentSourceOffset) {
  EntityHandle rootHandle = document.svgElement().entityHandle();
  if (!rootHandle) {
    return std::nullopt;
  }

  std::optional<components::StyleRuleAtSourceOffset> rule =
      components::StyleSystem().findStyleRuleAtSourceOffset(*rootHandle.registry(),
                                                            documentSourceOffset);
  if (!rule.has_value()) {
    return std::nullopt;
  }

  return ToPublicSourceRule(*rule);
}

}  // namespace donner::svg
