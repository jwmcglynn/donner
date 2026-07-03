#include "donner/svg/components/style/StyleSystem.h"

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/components/style/StyleComponent.h"

namespace donner::svg::components {

namespace {

struct ShadowedElementAdapter {
  ShadowedElementAdapter(Registry& registry, Entity treeEntity, Entity dataEntity)
      : registry_(registry), treeEntity_(treeEntity), dataEntity_(dataEntity) {}

  bool operator==(const ShadowedElementAdapter& other) const {
    return treeEntity_ == other.treeEntity_;
  }

  Entity entity() const { return treeEntity_; }

  std::optional<ShadowedElementAdapter> parentElement() const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).parent();

    const bool isSVGElement =
        (target != entt::null && registry_.get().all_of<ElementTypeComponent>(target));
    return isSVGElement ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> firstChild() const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).firstChild();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> lastChild() const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).lastChild();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> previousSibling() const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).previousSibling();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> nextSibling() const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).nextSibling();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  xml::XMLQualifiedName tagName() const {
    return registry_.get().get<donner::components::TreeComponent>(treeEntity_).tagName();
  }

  bool isKnownType() const {
    return registry_.get().get<ElementTypeComponent>(treeEntity_).type() !=
           svg::ElementType::Unknown;
  }

  RcString id() const {
    if (const auto* component = registry_.get().try_get<IdComponent>(dataEntity_)) {
      return component->id();
    } else {
      return "";
    }
  }

  RcString className() const {
    if (const auto* component = registry_.get().try_get<ClassComponent>(dataEntity_)) {
      return component->className;
    } else {
      return "";
    }
  }

  bool hasAttribute(const xml::XMLQualifiedNameRef& name) const {
    if (const auto* component =
            registry_.get().try_get<donner::components::AttributesComponent>(dataEntity_)) {
      return component->hasAttribute(name);
    } else {
      return false;
    }
  }

  std::optional<RcString> getAttribute(const xml::XMLQualifiedNameRef& name) const {
    if (const auto* component =
            registry_.get().try_get<donner::components::AttributesComponent>(dataEntity_)) {
      return component->getAttribute(name);
    } else {
      return std::nullopt;
    }
  }

  SmallVector<xml::XMLQualifiedNameRef, 1> findMatchingAttributes(
      const xml::XMLQualifiedNameRef& matcher) const {
    if (const auto* component =
            registry_.get().try_get<donner::components::AttributesComponent>(dataEntity_)) {
      return component->findMatchingAttributes(matcher);
    } else {
      return {};
    }
  }

private:
  ShadowedElementAdapter create(Entity newTreeEntity) const {
    const auto* shadowComponent = registry_.get().try_get<ShadowEntityComponent>(newTreeEntity);
    return ShadowedElementAdapter(registry_.get(), newTreeEntity,
                                  shadowComponent ? shadowComponent->lightEntity : newTreeEntity);
  }

  std::reference_wrapper<Registry> registry_;
  Entity treeEntity_;
  Entity dataEntity_;
};

struct MatchedSelectorEntry {
  std::size_t selectorEntryIndex = 0;
  css::Specificity specificity;
};

std::optional<MatchedSelectorEntry> MatchSelectorRule(const css::SelectorRule& rule,
                                                      const ShadowedElementAdapter& adapter) {
  for (std::size_t i = 0; i < rule.selector.entries.size(); ++i) {
    if (css::SelectorMatchResult match = rule.selector.entries[i].matches(
            adapter, css::SelectorMatchOptions<ShadowedElementAdapter>());
        match) {
      return MatchedSelectorEntry{
          .selectorEntryIndex = i,
          .specificity = match.specificity,
      };
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> ResolveOffset(const SourceRange& range, std::string_view source,
                                         bool end) {
  const FileOffset resolved = (end ? range.end : range.start).resolveOffset(source);
  return resolved.offset;
}

bool LocalRangeContainsOffset(const SourceRange& range, std::size_t localOffset) {
  const std::optional<std::size_t> start = ResolveOffset(range, std::string_view(), false);
  const std::optional<std::size_t> end = ResolveOffset(range, std::string_view(), true);
  return start.has_value() && end.has_value() && *start <= localOffset && localOffset < *end;
}

}  // namespace

const ComputedStyleComponent& StyleSystem::computeStyle(EntityHandle handle,
                                                        ParseWarningSink& warningSink) {
  auto& computedStyle = handle.get_or_emplace<ComputedStyleComponent>();
  computePropertiesInto(handle, computedStyle, warningSink);
  return computedStyle;
}

void StyleSystem::updateStyle(EntityHandle handle, std::string_view style) {
  // Update the PropertyRegistry with the new declarations.
  auto& styleComponent = handle.get_or_emplace<StyleComponent>();
  styleComponent.updateStyle(style);

  // Merge the new style string with the existing style attribute.
  auto& attributes = handle.get_or_emplace<donner::components::AttributesComponent>();
  const std::optional<RcString> existingStyleValue =
      attributes.getAttribute(xml::XMLQualifiedNameRef("style"));
  const std::string_view existingStyle =
      existingStyleValue.has_value() ? std::string_view(*existingStyleValue) : std::string_view();

  const std::vector<css::Declaration> existingDeclarations =
      css::CSS::ParseStyleAttribute(existingStyle);
  const std::vector<css::Declaration> updateDeclarations = css::CSS::ParseStyleAttribute(style);
  const std::string mergedStyle =
      css::mergeStyleDeclarations(existingDeclarations, updateDeclarations);

  attributes.setAttribute(*handle.registry(), xml::XMLQualifiedName("style"),
                          RcString(mergedStyle));

  invalidateComputed(handle);
}

void StyleSystem::computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                                        ParseWarningSink& warningSink) {
  if (computedStyle.properties) {
    return;  // Already computed.
  }

  Registry& registry = *handle.registry();

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();

  // Apply local style.
  PropertyRegistry properties;
  if (auto* styleComponent = registry.try_get<StyleComponent>(dataEntity)) {
    properties = styleComponent->properties;
  } else {
    properties = PropertyRegistry();
  }

  // Apply style from stylesheets.
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    auto [stylesheet] = view.get(stylesheetEntity);

    for (const css::SelectorRule& rule : stylesheet.stylesheet.rules()) {
      if (std::optional<MatchedSelectorEntry> match = MatchSelectorRule(
              rule, ShadowedElementAdapter(registry, handle.entity(), dataEntity));
          match.has_value()) {
        css::Specificity specificity = match->specificity;
        if (stylesheet.isUserAgentStylesheet) {
          specificity = specificity.toUserAgentSpecificity();
        }

        for (const auto& declaration : rule.declarations) {
          if (auto error = properties.parseProperty(declaration, specificity)) {
            warningSink.add(std::move(error.value()));
          }
        }
      }
    }
  }

  // Inherit from parent.
  const Entity parent = handle.get<donner::components::TreeComponent>().parent();
  if (parent != entt::null) {
    auto& parentStyleComponent = registry.get_or_emplace<ComputedStyleComponent>(parent);
    computePropertiesInto(EntityHandle(registry, parent), parentStyleComponent, warningSink);

    // <pattern> elements can't inherit 'fill' or 'stroke' or it creates recursion in the shadow
    // tree.
    const PropertyInheritOptions inheritOptions =
        registry.all_of<DoNotInheritFillOrStrokeTag>(parent) ? PropertyInheritOptions::NoPaint
                                                             : PropertyInheritOptions::All;

    computedStyle.properties =
        properties.inheritFrom(parentStyleComponent.properties.value(), inheritOptions);
  } else {
    computedStyle.properties = properties;
  }

  // Resolve font-size to absolute pixels. CSS spec requires the computed value of font-size to
  // always be an absolute length. Relative units (em, %, ex) resolve against the parent's computed
  // font-size, and percentages resolve against the parent font-size (not the viewBox).
  if (computedStyle.properties) {
    double parentFontSizePx = 12.0;  // UA default font size (medium) for root elements.
    if (parent != entt::null) {
      const auto& parentStyle = registry.get<ComputedStyleComponent>(parent);
      if (parentStyle.properties) {
        parentFontSizePx = parentStyle.properties->fontSize.get().value().value;
      }
    }
    computedStyle.properties->resolveFontSize(parentFontSizePx);

    // Resolve relative font-weight keywords (bolder/lighter) against inherited weight.
    int parentFontWeight = 400;  // CSS initial value.
    if (parent != entt::null) {
      const auto& parentStyle = registry.get<ComputedStyleComponent>(parent);
      if (parentStyle.properties) {
        parentFontWeight = parentStyle.properties->fontWeight.get().value();
      }
    }
    computedStyle.properties->resolveFontWeight(parentFontWeight);

    // Resolve relative font-stretch keywords (narrower/wider) against inherited stretch.
    int parentFontStretch = static_cast<int>(FontStretch::Normal);
    if (parent != entt::null) {
      const auto& parentStyle = registry.get<ComputedStyleComponent>(parent);
      if (parentStyle.properties) {
        parentFontStretch = parentStyle.properties->fontStretch.get().value();
      }
    }
    computedStyle.properties->resolveFontStretch(parentFontStretch);
  }
}

std::vector<MatchedStyleRule> StyleSystem::collectMatchedStyleRules(EntityHandle handle) const {
  Registry& registry = *handle.registry();

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();

  std::vector<MatchedStyleRule> result;
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    const auto& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
    const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();

    for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
      const css::SelectorRule& rule = rules[ruleIndex];
      std::optional<MatchedSelectorEntry> match =
          MatchSelectorRule(rule, ShadowedElementAdapter(registry, handle.entity(), dataEntity));
      if (!match.has_value()) {
        continue;
      }

      css::Specificity specificity = match->specificity;
      if (stylesheet.isUserAgentStylesheet) {
        specificity = specificity.toUserAgentSpecificity();
      }

      MatchedStyleRule matchedRule;
      matchedRule.stylesheetEntity = stylesheetEntity;
      matchedRule.ruleIndex = ruleIndex;
      matchedRule.selectorEntryIndex = match->selectorEntryIndex;
      matchedRule.specificity = specificity;
      matchedRule.isUserAgentStylesheet = stylesheet.isUserAgentStylesheet;
      matchedRule.ruleSourceRange = stylesheet.sourceMap.mapToDocumentSource(rule.ruleSourceRange);

      if (match->selectorEntryIndex < rule.selectorEntrySourceRanges.size()) {
        matchedRule.selectorSourceRange = stylesheet.sourceMap.mapToDocumentSource(
            rule.selectorEntrySourceRanges[match->selectorEntryIndex]);
      } else {
        matchedRule.selectorSourceRange =
            stylesheet.sourceMap.mapToDocumentSource(rule.selectorSourceRange);
      }

      for (const css::Declaration& declaration : rule.declarations) {
        if (std::optional<SourceRange> sourceRange =
                stylesheet.sourceMap.mapToDocumentSource(declaration.sourceRange)) {
          matchedRule.declarationSourceRanges.push_back(*sourceRange);
        }
      }

      result.push_back(std::move(matchedRule));
    }
  }

  return result;
}

std::optional<StyleRuleAtSourceOffset> StyleSystem::findStyleRuleAtSourceOffset(
    Registry& registry, std::size_t documentSourceOffset) const {
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    const auto& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
    if (stylesheet.isUserAgentStylesheet || stylesheet.sourceMap.empty()) {
      continue;
    }

    std::optional<std::size_t> localOffset =
        stylesheet.sourceMap.mapToLocalCssOffset(documentSourceOffset);
    if (!localOffset.has_value()) {
      continue;
    }

    const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();
    for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
      const css::SelectorRule& rule = rules[ruleIndex];
      if (!LocalRangeContainsOffset(rule.ruleSourceRange, *localOffset)) {
        continue;
      }

      std::optional<SourceRange> ruleSourceRange =
          stylesheet.sourceMap.mapToDocumentSource(rule.ruleSourceRange);
      std::optional<SourceRange> selectorSourceRange =
          stylesheet.sourceMap.mapToDocumentSource(rule.selectorSourceRange);
      if (!ruleSourceRange.has_value() || !selectorSourceRange.has_value()) {
        continue;
      }

      StyleRuleAtSourceOffset result;
      result.stylesheetEntity = stylesheetEntity;
      result.ruleIndex = ruleIndex;
      result.ruleSourceRange = *ruleSourceRange;
      result.selectorSourceRange = *selectorSourceRange;

      for (std::size_t selectorEntryIndex = 0;
           selectorEntryIndex < rule.selectorEntrySourceRanges.size(); ++selectorEntryIndex) {
        if (!LocalRangeContainsOffset(rule.selectorEntrySourceRanges[selectorEntryIndex],
                                      *localOffset)) {
          continue;
        }

        result.selectorEntryIndex = selectorEntryIndex;
        if (std::optional<SourceRange> entrySourceRange = stylesheet.sourceMap.mapToDocumentSource(
                rule.selectorEntrySourceRanges[selectorEntryIndex])) {
          result.selectorSourceRange = *entrySourceRange;
        }
        break;
      }

      return result;
    }
  }

  return std::nullopt;
}

void StyleSystem::computeAllStyles(Registry& registry, ParseWarningSink& warningSink) {
  const auto* renderState = registry.ctx().find<RenderTreeState>();
  const bool hasBeenBuilt = renderState != nullptr && renderState->hasBeenBuilt;
  const bool needsFullStyleRecompute =
      renderState != nullptr && renderState->needsFullStyleRecompute;
  const bool hasDirtyEntities = !registry.view<DirtyFlagsComponent>().empty();

  if (hasBeenBuilt && hasDirtyEntities && !needsFullStyleRecompute) {
    for (auto entity : registry.view<DirtyFlagsComponent>()) {
      const auto& dirty = registry.get<DirtyFlagsComponent>(entity);
      if (!dirty.test(DirtyFlagsComponent::Style)) {
        continue;
      }

      std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
      computeStyle(EntityHandle(registry, entity), warningSink);
    }

    ResourceManagerContext& resourceManager = registry.ctx().get<ResourceManagerContext>();
    for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
      auto [stylesheet] = view.get(stylesheetEntity);
      resourceManager.addFontFaces(stylesheet.stylesheet.fontFaces());
    }

    return;
  }

  if (hasBeenBuilt && needsFullStyleRecompute) {
    registry.clear<ComputedStyleComponent>();
  }

  // Create placeholder ComputedStyleComponents for all elements in the range, since creating
  // computed style components also creates the parents, and we can't modify the component list
  // while iterating it.
  auto view = registry.view<donner::components::TreeComponent>();
  for (auto entity : view) {
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }

  // Compute the styles for all elements.
  for (auto entity : view) {
    computeStyle(EntityHandle(registry, entity), warningSink);
  }

  ResourceManagerContext& resourceManager = registry.ctx().get<ResourceManagerContext>();
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    auto [stylesheet] = view.get(stylesheetEntity);

    resourceManager.addFontFaces(stylesheet.stylesheet.fontFaces());
  }
}

void StyleSystem::computeStylesFor(Registry& registry, std::span<const Entity> entities,
                                   ParseWarningSink& warningSink) {
  for (Entity entity : entities) {
    computeStyle(EntityHandle(registry, entity), warningSink);
  }
}

void StyleSystem::invalidateComputed(EntityHandle handle) {
  handle.remove<ComputedStyleComponent>();
}

void StyleSystem::invalidateAll(EntityHandle handle) {
  invalidateComputed(handle);
  // TODO: Reparse attributes.
}

}  // namespace donner::svg::components
