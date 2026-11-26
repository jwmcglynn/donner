#include "donner/svg/SVGStyleQuery.h"

#include <span>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg {
namespace {

using components::StylesheetComponent;

std::optional<SVGStyleElement> EmbeddedStylesheet(Registry& registry, Entity entity) {
  return SVGElement::CreateFromEntityHandle(EntityHandle(registry, entity))
      .tryCast<SVGStyleElement>();
}

SVGMatchedStyleRule ToPublicMatchedRule(Registry& registry,
                                        const components::MatchedStyleRule& rule) {
  return SVGMatchedStyleRule{
      .stylesheetEntity = rule.stylesheetEntity,
      .ruleIndex = rule.ruleIndex,
      .selectorEntryIndex = rule.selectorEntryIndex,
      .specificity = rule.specificity,
      .isUserAgentStylesheet = rule.isUserAgentStylesheet,
      .stylesheetElement = EmbeddedStylesheet(registry, rule.stylesheetEntity),
      .ruleCssRange = rule.ruleCssRange,
      .selectorCssRange = rule.selectorCssRange,
      .ruleSourceRange = rule.ruleSourceRange,
      .selectorSourceRange = rule.selectorSourceRange,
      .declarationSourceRanges = rule.declarationSourceRanges,
  };
}

SVGStylesheetRule ToPublicStylesheetRule(Registry& registry, Entity stylesheetEntity,
                                         std::size_t ruleIndex,
                                         const StylesheetComponent& stylesheet,
                                         const css::SelectorRule& rule) {
  const auto project = [&](const SourceRange& range) {
    return components::StyleSystem().mapStylesheetSourceRange(registry, stylesheetEntity, range);
  };
  SVGStylesheetRule result{
      .isUserAgentStylesheet = stylesheet.isUserAgentStylesheet,
      .stylesheetEntity = stylesheetEntity,
      .ruleIndex = ruleIndex,
      .stylesheetElement = EmbeddedStylesheet(registry, stylesheetEntity),
      .ruleCssRange = rule.ruleSourceRange,
      .selectorCssRange = rule.selectorSourceRange,
      .ruleSourceRange = project(rule.ruleSourceRange),
      .selectorSourceRange = project(rule.selectorSourceRange),
  };
  result.declarations.reserve(rule.declarations.size());
  for (std::size_t declarationIndex = 0; declarationIndex < rule.declarations.size();
       ++declarationIndex) {
    const css::Declaration& declaration = rule.declarations[declarationIndex];
    result.declarations.push_back(SVGStylesheetDeclaration{
        .declarationIndex = declarationIndex,
        .declaration = declaration,
        .declarationSourceRange =
            project(declaration.authoredDeclarationRange().value_or(declaration.sourceRange)),
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
      rules.push_back(ToPublicMatchedRule(*handle.registry(), rule));
    }
    return rules;
  });
}

std::vector<SVGStylesheetRule> CollectStylesheetRules(const SVGDocument& document,
                                                      bool includeUserAgent) {
  return document.withReadAccess([includeUserAgent](DocumentReadAccess& access) {
    std::vector<SVGStylesheetRule> result;
    Registry& registry = access.registry();
    for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
      const StylesheetComponent& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
      if (!stylesheet.isCssType()) {
        continue;
      }
      if (stylesheet.isUserAgentStylesheet && !includeUserAgent) {
        continue;
      }

      const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();
      for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
        auto rule = ToPublicStylesheetRule(registry, stylesheetEntity, ruleIndex, stylesheet,
                                           rules[ruleIndex]);
        if (stylesheet.isUserAgentStylesheet || rule.ruleSourceRange) {
          result.push_back(std::move(rule));
        }
      }
    }
    return result;
  });
}

bool AuthorStyleDependsOnTreePosition(const SVGDocument& document) {
  return document.withReadAccess([](DocumentReadAccess& access) {
    Registry& registry = access.registry();
    for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
      const StylesheetComponent& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
      if (!stylesheet.isCssType() || stylesheet.isUserAgentStylesheet) {
        continue;
      }

      for (const css::SelectorRule& rule : stylesheet.stylesheet.rules()) {
        if (rule.selector.dependsOnTreePosition()) {
          return true;
        }
      }
    }

    return false;
  });
}

std::optional<SourceRange> MapStylesheetSourceRange(const SVGStyleElement& stylesheet,
                                                    const SourceRange& cssRange) {
  return stylesheet.withReadAccess([&](DocumentReadAccess& access, EntityHandle handle) {
    return components::StyleSystem().mapStylesheetSourceRange(access.registry(), handle.entity(),
                                                              cssRange);
  });
}

std::optional<SourceRange> FindCustomPropertyDeclaration(const SVGElement& element,
                                                         std::string_view name) {
  return element.withReadAccess(
      [&](DocumentReadAccess&, EntityHandle handle) -> std::optional<SourceRange> {
        if (!handle || name.size() <= 2 || !name.starts_with("--")) {
          return std::nullopt;
        }
        return components::StyleSystem().findCustomPropertyDeclaration(handle, name);
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
