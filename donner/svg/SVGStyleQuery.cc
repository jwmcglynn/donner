#include "donner/svg/SVGStyleQuery.h"

#include <span>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg {
namespace {

using components::StylesheetComponent;

SVGMatchedStyleRule ToPublicMatchedRule(const components::MatchedStyleRule& rule) {
  return SVGMatchedStyleRule{
      .stylesheetEntity = rule.stylesheetEntity,
      .ruleIndex = rule.ruleIndex,
      .selectorEntryIndex = rule.selectorEntryIndex,
      .specificity = rule.specificity,
      .isUserAgentStylesheet = rule.isUserAgentStylesheet,
      .ruleSourceRange = rule.ruleSourceRange,
      .selectorSourceRange = rule.selectorSourceRange,
      .declarationSourceRanges = rule.declarationSourceRanges,
  };
}

SVGStylesheetRule ToPublicStylesheetRule(Entity stylesheetEntity, std::size_t ruleIndex,
                                         const StylesheetComponent& stylesheet,
                                         const css::SelectorRule& rule) {
  SVGStylesheetRule result{
      .stylesheetEntity = stylesheetEntity,
      .ruleIndex = ruleIndex,
      .ruleSourceRange = stylesheet.sourceMap.mapToDocumentSource(rule.ruleSourceRange),
      .selectorSourceRange = stylesheet.sourceMap.mapToDocumentSource(rule.selectorSourceRange),
  };
  result.declarations.reserve(rule.declarations.size());
  for (std::size_t declarationIndex = 0; declarationIndex < rule.declarations.size();
       ++declarationIndex) {
    const css::Declaration& declaration = rule.declarations[declarationIndex];
    result.declarations.push_back(SVGStylesheetDeclaration{
        .declarationIndex = declarationIndex,
        .declaration = declaration,
        .declarationSourceRange = stylesheet.sourceMap.mapToDocumentSource(declaration.sourceRange),
    });
  }
  return result;
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
  return element.withReadAccess([](DocumentReadAccess&, EntityHandle handle) {
    if (!handle) {
      return std::vector<SVGMatchedStyleRule>();
    }

    std::vector<components::MatchedStyleRule> internalRules =
        components::StyleSystem().collectMatchedStyleRules(handle);
    std::vector<SVGMatchedStyleRule> rules;
    rules.reserve(internalRules.size());
    for (const components::MatchedStyleRule& rule : internalRules) {
      rules.push_back(ToPublicMatchedRule(rule));
    }
    return rules;
  });
}

std::vector<SVGStylesheetRule> CollectStylesheetRules(const SVGDocument& document) {
  return document.withReadAccess([](DocumentReadAccess& access) {
    std::vector<SVGStylesheetRule> result;
    Registry& registry = access.registry();
    for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
      const StylesheetComponent& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
      if (stylesheet.isUserAgentStylesheet || stylesheet.sourceMap.empty()) {
        continue;
      }

      const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();
      for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
        result.push_back(
            ToPublicStylesheetRule(stylesheetEntity, ruleIndex, stylesheet, rules[ruleIndex]));
      }
    }
    return result;
  });
}

std::optional<SVGStyleRuleAtSourceOffset> FindStyleRuleAtSourceOffset(
    const SVGDocument& document, std::size_t documentSourceOffset) {
  return document.withReadAccess([&document, documentSourceOffset](DocumentReadAccess&) {
    EntityHandle rootHandle = document.svgElement().entityHandle();
    if (!rootHandle) {
      return std::optional<SVGStyleRuleAtSourceOffset>();
    }

    std::optional<components::StyleRuleAtSourceOffset> rule =
        components::StyleSystem().findStyleRuleAtSourceOffset(*rootHandle.registry(),
                                                              documentSourceOffset);
    if (!rule.has_value()) {
      return std::optional<SVGStyleRuleAtSourceOffset>();
    }

    return std::optional<SVGStyleRuleAtSourceOffset>(ToPublicSourceRule(*rule));
  });
}

}  // namespace donner::svg
