#include "donner/svg/SVGStyleQuery.h"

#include <array>
#include <span>

#include "donner/base/xml/XMLNode.h"
#include "donner/svg/ElementFactory.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg {
namespace {

using components::StylesheetComponent;

std::optional<SVGStyleElement> EmbeddedStylesheet(Registry& registry, Entity entity) {
  return details::ElementFactory::TryWrapStyle(EntityHandle(registry, entity));
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
                                         const css::SelectorRule& rule,
                                         bool includeDeclarations = true) {
  const auto project = [&](const SourceRange& range) {
    return components::StyleSystem().mapStylesheetSourceRange(registry, stylesheetEntity, range);
  };
  SVGStylesheetRule result{
      .blockClosedAtEof = rule.blockClosedAtEof,
      .isUserAgentStylesheet = stylesheet.isUserAgentStylesheet,
      .stylesheetEntity = stylesheetEntity,
      .ruleIndex = ruleIndex,
      .stylesheetElement = EmbeddedStylesheet(registry, stylesheetEntity),
      .ruleCssRange = rule.ruleSourceRange,
      .selectorCssRange = rule.selectorSourceRange,
      .ruleSourceRange = project(rule.ruleSourceRange),
      .selectorSourceRange = project(rule.selectorSourceRange),
  };
  if (!includeDeclarations) {
    return result;
  }
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

bool ContainsSourceOffset(const SourceRange& range, std::size_t offset) {
  return range.start.offset && range.end.offset && *range.start.offset <= offset &&
         offset < *range.end.offset;
}

std::optional<SVGStylesheetRule> FindProjectedRuleAtOffset(Registry& registry, std::size_t offset,
                                                           bool includeDeclarations) {
  const auto& styleSystem = components::StyleSystem();
  for (auto sheets = registry.view<StylesheetComponent>(); const Entity entity : sheets) {
    const auto& sheet = sheets.get<StylesheetComponent>(entity);
    if (sheet.isUserAgentStylesheet || !sheet.isCssType()) {
      continue;
    }
    const auto node = xml::XMLNode::TryCast(EntityHandle(registry, entity));
    const auto location = node ? node->getNodeLocation() : std::nullopt;
    if (!location || !ContainsSourceOffset(*location, offset)) {
      continue;
    }
    const auto rules = sheet.stylesheet.rules();
    for (std::size_t index = 0; index < rules.size(); ++index) {
      const auto range =
          styleSystem.mapStylesheetSourceRange(registry, entity, rules[index].ruleSourceRange);
      if (range && ContainsSourceOffset(*range, offset)) {
        return ToPublicStylesheetRule(registry, entity, index, sheet, rules[index],
                                      includeDeclarations);
      }
    }
  }
  return std::nullopt;
}

}  // namespace

bool VisitAuthoredStylesheetDeclarations(
    const SVGStyleElement& stylesheet, std::size_t ruleIndex, const SourceRange& expectedRuleRange,
    const std::function<void(std::string_view, std::span<const css::Declaration>)>& visit) {
  return stylesheet.withReadAccess([&](DocumentReadAccess&, EntityHandle handle) {
    const auto* sheet = handle.try_get<StylesheetComponent>();
    if (!sheet || sheet->isUserAgentStylesheet || !sheet->isCssType() ||
        ruleIndex >= sheet->stylesheet.rules().size()) {
      return false;
    }
    const auto& rule = sheet->stylesheet.rules()[ruleIndex];
    if (rule.ruleSourceRange != expectedRuleRange) {
      return false;
    }
    visit(sheet->text, rule.declarations);
    return true;
  });
}

std::optional<bool> HasPreparedCustomPropertyValue(const SVGElement& element,
                                                   std::string_view name) {
  return element.withReadAccess([&](DocumentReadAccess&, EntityHandle handle) {
    return components::StyleSystem::preparedCustomPropertyHasValue(handle, name);
  });
}

std::vector<SVGMatchedStyleRule> CollectMatchedStyleRules(const SVGElement& element,
                                                          bool includeDeclarations) {
  return element.withReadAccess([includeDeclarations](DocumentReadAccess&, EntityHandle handle) {
    if (!handle) {
      return std::vector<SVGMatchedStyleRule>();
    }

    std::vector<components::MatchedStyleRule> internalRules =
        components::StyleSystem().collectMatchedStyleRules(handle, includeDeclarations);
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

std::optional<SVGStylesheetRule> CollectStylesheetRuleAtSourceOffset(
    const SVGDocument& document, std::size_t documentSourceOffset, bool includeDeclarations) {
  return document.withReadAccess(
      [&](DocumentReadAccess& access) -> std::optional<SVGStylesheetRule> {
        Registry& registry = access.registry();
        const auto found =
            components::StyleSystem().findStyleRuleAtSourceOffset(registry, documentSourceOffset);
        if (!found) {
          return FindProjectedRuleAtOffset(registry, documentSourceOffset, includeDeclarations);
        }
        const auto& sheet = registry.get<StylesheetComponent>(found->stylesheetEntity);
        auto rule =
            ToPublicStylesheetRule(registry, found->stylesheetEntity, found->ruleIndex, sheet,
                                   sheet.stylesheet.rules()[found->ruleIndex], includeDeclarations);
        return rule.ruleSourceRange ? std::optional(std::move(rule)) : std::nullopt;
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

SVGCustomPropertyDeclarationBatch FindCustomPropertyDeclarations(
    const SVGElement& element, std::span<const std::string_view> names, std::size_t maxWork) {
  return element.withReadAccess([&](DocumentReadAccess&, EntityHandle handle) {
    auto internal =
        components::StyleSystem().findCustomPropertyDeclarations(handle, names, maxWork);
    return SVGCustomPropertyDeclarationBatch{
        .declarations = std::move(internal.declarations),
        .work = internal.work,
        .budgetExceeded = internal.budgetExceeded,
        .complete = internal.complete,
    };
  });
}

std::optional<SourceRange> FindCustomPropertyDeclaration(const SVGElement& element,
                                                         std::string_view name) {
  const std::array names{name};
  auto result = FindCustomPropertyDeclarations(
      element, names, components::StyleSystem::kMaximumCustomPropertyDeclarationQueryWork);
  return result.complete && !result.declarations.empty() ? result.declarations.front()
                                                         : std::nullopt;
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
