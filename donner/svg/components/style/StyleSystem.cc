#include "donner/svg/components/style/StyleSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/StringUtils.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/PaintAnimationStyle.h"
#include "donner/svg/components/style/ScalarAnimationStyle.h"
#include "donner/svg/components/style/SelectorContent.h"
#include "donner/svg/components/style/StyleComponent.h"

namespace donner::svg::components {

namespace {

/// Root computed font size for rem lengths resolved before descendants inherit them.
double ComputedRootFontSize(const Registry& registry) {
  const auto* context = registry.ctx().find<SVGDocumentContext>();
  if (context && registry.valid(context->rootEntity)) {
    // Installed root properties avoid re-entering style computation for root-owned rem.
    if (const auto* root = registry.try_get<ComputedStyleComponent>(context->rootEntity);
        root && root->properties) {
      return root->properties->fontSize.get().value().value;
    }
  }
  return FontMetrics().rootFontSize;
}

void SynchronizeStylesheetFonts(ResourceManagerContext& resources, Registry& registry,
                                Entity entity, const StylesheetComponent& stylesheet) {
  const css::Stylesheet empty;
  resources.synchronizeStylesheetFontFaces(entity, HasSvgNamespace(registry, entity)
                                                       ? stylesheet.stylesheet.fontFaces()
                                                       : empty.fontFaces());
}

bool IsActiveAuthorStylesheet(const Registry& registry, Entity entity,
                              const StylesheetComponent& stylesheet) {
  return HasSvgNamespace(registry, entity) && !stylesheet.isUserAgentStylesheet &&
         !stylesheet.sourceMap.empty();
}

bool IsApplicableStylesheet(const Registry& registry, const StylesheetComponent& stylesheet,
                            Entity stylesheetEntity, Entity dataEntity) {
  if (!HasSvgNamespace(registry, stylesheetEntity)) {
    return false;
  }
  if (!stylesheet.isUserAgentStylesheet) {
    return true;
  }
  // The built-in sheet has an SVG default namespace; authored namespace handling is separate.
  const auto* type = registry.try_get<ElementTypeComponent>(dataEntity);
  return type && type->inSvgNamespace();
}

StyleResourceBudget& GetStyleResourceBudget(Registry& registry) {
  if (registry.ctx().contains<StyleResourceBudget>()) {
    auto& budget = registry.ctx().get<StyleResourceBudget>();
    const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
    budget.setAggregateWorkBudget(occurrence ? occurrence->work.get() : nullptr);
    return budget;
  }

  std::shared_ptr<DocumentResourceFamilyBudget> family;
  if (const auto* context = registry.ctx().find<DocumentResourceFamilyContext>()) {
    family = context->budget;
  }
  auto& budget = registry.ctx().emplace<StyleResourceBudget>(std::move(family));
  const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
  budget.setAggregateWorkBudget(occurrence ? occurrence->work.get() : nullptr);
  return budget;
}

bool InheritsExternalPaint(PropertyState state) {
  return state == PropertyState::NotSet || state == PropertyState::Inherit ||
         state == PropertyState::ExplicitUnset;
}

template <typename T, PropertyCascade cascade>
Property<T, cascade> InheritExternalProperty(const Property<T, cascade>& local,
                                             const Property<T, cascade>& parent) {
  const bool inherited = cascade != PropertyCascade::None;
  if (local.state != PropertyState::Inherit && !(inherited && InheritsExternalPaint(local.state))) {
    return local;
  }
  Property<T, cascade> result = local;
  result.value = parent.get();
  result.state = PropertyState::Set;
  result.specificity = css::Specificity().toUserAgentSpecificity();
  return result;
}

template <PropertyCascade cascade>
bool BlockExternalReference(Property<Reference, cascade>& property,
                            const Property<Reference, cascade>& local) {
  const bool inherits = cascade == PropertyCascade::None ? local.state == PropertyState::Inherit
                                                         : InheritsExternalPaint(local.state);
  const auto reference = property.get();
  if (!inherits || !reference || reference->href.empty()) {
    return false;
  }
  property.set(PropertyState::ExplicitInitial, css::Specificity());
  return true;
}

bool BlockExternalClipPath(PropertyRegistry& properties, const PropertyRegistry& local) {
  const ClipPath* clip = properties.clipPath.getStoredValue();
  if (local.clipPath.state != PropertyState::Inherit || !clip) {
    return false;
  }
  const auto* reference = std::get_if<Reference>(&clip->value);
  if (!reference || reference->href.empty()) {
    return false;
  }
  properties.clipPath.set(PropertyState::ExplicitInitial, css::Specificity());
  return true;
}

bool BlockExternalResources(PropertyRegistry& properties, const PropertyRegistry& local) {
  bool rejected = BlockExternalReference(properties.markerStart, local.markerStart);
  rejected |= BlockExternalReference(properties.markerMid, local.markerMid);
  rejected |= BlockExternalReference(properties.markerEnd, local.markerEnd);
  rejected |= BlockExternalReference(properties.mask, local.mask);
  rejected |= BlockExternalClipPath(properties, local);
  if (local.filter.state == PropertyState::Inherit) {
    const auto* effects = properties.filter.getStoredValue();
    if (effects && std::any_of(effects->begin(), effects->end(), [](const FilterEffect& effect) {
          return effect.is<FilterEffect::ElementReference>();
        })) {
      properties.filter.set(PropertyState::ExplicitInitial, css::Specificity());
      rejected = true;
    }
  }
  if (InheritsExternalPaint(local.cursor.state) &&
      !properties.cursor.get().value().images.empty()) {
    properties.cursor.set(PropertyState::ExplicitInitial, css::Specificity());
    rejected = true;
  }
  return rejected;
}

const ExternalUseStyleContext* ExternalRootContext(Registry& registry, Entity entity) {
  const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
  return occurrence && occurrence->root == entity ? occurrence : nullptr;
}

bool IsDirectUseInstanceRoot(EntityHandle handle) {
  if (ExternalRootContext(*handle.registry(), handle.entity())) {
    return true;
  }
  const auto* root = handle.try_get<ShadowTreeRootComponent>();
  if (!root || !handle.all_of<ShadowEntityComponent>()) {
    return false;
  }
  Entity source = root->sourceEntity;
  if (const auto* shadow = handle.registry()->try_get<ShadowEntityComponent>(source)) {
    source = shadow->lightEntity;
  }
  const auto* type = handle.registry()->try_get<ElementTypeComponent>(source);
  return type && type->type() == ElementType::Use;
}

void ApplySymbolInstanceDisplay(EntityHandle handle, Entity dataEntity,
                                PropertyRegistry& properties) {
  const auto* type = handle.registry()->try_get<ElementTypeComponent>(dataEntity);
  if (!type || type->type() != ElementType::Symbol || !IsDirectUseInstanceRoot(handle)) {
    return;
  }
  const css::Specificity priority =
      css::Specificity::Important(css::Specificity::FromABC(0, 0, 1).toUserAgentSpecificity());
  if (properties.display.specificity <= priority) {
    properties.display.set(Display::Inline, priority);
  }
}

bool ExternalPaintOrigin(PropertyState local, bool root, const ComputedStyleComponent* parent,
                         bool stroke, bool allowPaint) {
  if (!allowPaint || !InheritsExternalPaint(local)) {
    return false;
  }
  return root || (parent && (stroke ? parent->externalStroke : parent->externalFill));
}

void ApplyAnimatedStyle(Registry& registry, Entity dataEntity, PropertyRegistry& properties) {
  if (const auto* animationStyle = registry.ctx().find<AnimationStyleContext>();
      animationStyle && animationStyle->enabled) {
    if (const auto* animated = registry.try_get<AnimatedValuesComponent>(dataEntity)) {
      for (const auto& [name, value] : animated->overrides) {
        std::ignore = properties.parsePresentationAttribute(name, value, EntityHandle(),
                                                            css::Specificity::Animation());
      }
    }
  }
}

std::optional<Lengthd> ResolveFontOperand(std::string_view value, double parentSize) {
  PropertyRegistry parsed;
  const auto result = parsed.parsePresentationAttribute("font-size", value, EntityHandle(),
                                                        css::Specificity::Animation());
  if (result.hasError() || !result.result()) {
    return std::nullopt;
  }
  if (parsed.fontSize.state == PropertyState::Inherit ||
      parsed.fontSize.state == PropertyState::ExplicitUnset) {
    parsed.fontSize.set(Lengthd(parentSize, Lengthd::Unit::Px), css::Specificity::Animation());
  }
  parsed.resolveFontSize(parentSize);
  const Lengthd resolved = parsed.fontSize.get().value();
  return std::isfinite(resolved.value) && resolved.value >= 0 ? std::optional(resolved)
                                                              : std::nullopt;
}

std::optional<Lengthd> ResolveFontDelta(std::string_view value, double parentSize) {
  const std::string_view trimmed = StringUtils::TrimWhitespace(value);
  donner::parser::LengthParser::Options options;
  options.unitOptional = true;
  const auto parsed = donner::parser::LengthParser::Parse(trimmed, options);
  if (parsed.hasResult() && parsed.result().consumedChars == trimmed.size() &&
      parsed.result().length.value < 0) {
    Lengthd magnitude = parsed.result().length;
    magnitude.value = -magnitude.value;
    auto resolved = ResolveFontOperand(magnitude.toRcString().str(), parentSize);
    if (resolved) {
      resolved->value = -resolved->value;
    }
    return resolved;
  }
  return ResolveFontOperand(value, parentSize);
}

bool NormalizeFontOperand(std::optional<std::string>& value, double parentSize,
                          bool delta = false) {
  if (!value) {
    return true;
  }
  const auto resolved =
      delta ? ResolveFontDelta(*value, parentSize) : ResolveFontOperand(*value, parentSize);
  if (!resolved) {
    return false;
  }
  value = resolved->toRcString().str();
  return true;
}

bool NormalizeFontDeclaration(AnimateValueComponent& declaration, double parentSize) {
  const bool toOnly = declaration.values.empty() && !declaration.from && declaration.to;
  const bool delta = declaration.additive && !toOnly;
  if (declaration.values.empty()) {
    if (declaration.to) {
      return NormalizeFontOperand(declaration.from, parentSize, delta) &&
             NormalizeFontOperand(declaration.to, parentSize, delta);
    }
    return NormalizeFontOperand(declaration.from, parentSize, delta) &&
           NormalizeFontOperand(declaration.by, parentSize, true);
  }
  for (auto& value : declaration.values) {
    const auto resolved =
        delta ? ResolveFontDelta(value, parentSize) : ResolveFontOperand(value, parentSize);
    if (!resolved) {
      return false;
    }
    value = resolved->toRcString().str();
  }
  return true;
}

std::optional<Lengthd> SampleFontLength(const ContextualLengthSample& sample, Lengthd underlying,
                                        double parentSize) {
  ContextualLengthSample normalized = sample;
  const bool valid = std::visit(
      [&](auto& declaration) {
        if constexpr (std::is_same_v<std::decay_t<decltype(declaration)>, AnimateValueComponent>) {
          return NormalizeFontDeclaration(declaration, parentSize);
        } else {
          return NormalizeFontOperand(declaration.to, parentSize);
        }
      },
      normalized.declaration);
  if (!valid) {
    return std::nullopt;
  }
  const auto sampled =
      SampleAnimationValue(sample.progress, sample.iteration,
                           std::get_if<SetAnimationComponent>(&normalized.declaration),
                           std::get_if<AnimateValueComponent>(&normalized.declaration), nullptr,
                           underlying.toRcString().str(), AnimationValueContext());
  return sampled ? ResolveFontOperand(sampled->second, parentSize) : std::nullopt;
}

Lengthd BaseFontLength(const PropertyRegistry& properties, double parentSize) {
  switch (properties.fontSize.state) {
    case PropertyState::NotSet:
    case PropertyState::Inherit:
    case PropertyState::ExplicitUnset: return Lengthd(parentSize, Lengthd::Unit::Px);
    default:
      return ResolveFontOperand(properties.fontSize.get().value().toRcString().str(), parentSize)
          .value_or(Lengthd(12, Lengthd::Unit::Px));
  }
}

void ApplyContextualFontSize(Registry& registry, Entity dataEntity,
                             const ComputedStyleComponent* parentStyle,
                             PropertyRegistry& properties) {
  const auto* animationStyle = registry.ctx().find<AnimationStyleContext>();
  const auto* animated = registry.try_get<AnimatedValuesComponent>(dataEntity);
  if (!animationStyle || !animationStyle->enabled || !animated) {
    return;
  }
  const auto recipe = animated->contextualLengths.find("font-size");
  if (recipe == animated->contextualLengths.end()) {
    return;
  }
  auto& budget = registry.ctx().get<AnimationResourceBudget>();
  if (!budget.reserveContextualWork(recipe->second->evaluationWork)) {
    return;
  }
  double parentSize = 12;
  if (parentStyle && parentStyle->properties) {
    parentSize = parentStyle->properties->fontSize.get().value().value;
  }
  Lengthd underlying = BaseFontLength(properties, parentSize);
  const auto result = SampleContextualAnimationRecipe(
      *recipe->second, underlying, [&](const ContextualLengthSample& sample, Lengthd lower) {
        return SampleFontLength(sample, lower, parentSize);
      });
  if (result) {
    std::ignore = properties.parsePresentationAttribute(
        "font-size", result->toRcString().str(), EntityHandle(), css::Specificity::Animation());
  }
}

template <typename PropertyType>
void PreserveLocalValue(const PropertyType& local, PropertyType& inherited) {
  if (local.state == PropertyState::Set || local.state == PropertyState::ExplicitInitial) {
    inherited = local;
  }
}

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
    if (target == entt::null) {
      return std::nullopt;
    }

    // Shadow entities don't carry ElementTypeComponent themselves - resolve through the light
    // (data) entity, otherwise any shadow entity whose parent is also a shadow entity would
    // appear parentless and incorrectly match `:root`.
    const bool isSVGElement = registry_.get().all_of<ElementTypeComponent>(resolveData(target));
    return isSVGElement ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> firstChild(
      css::SelectorTraversalBudget* budget = nullptr) const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).firstChild();
    return findElementSibling(target, true, budget);
  }

  std::optional<ShadowedElementAdapter> lastChild(
      css::SelectorTraversalBudget* budget = nullptr) const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).lastChild();
    return findElementSibling(target, false, budget);
  }

  std::optional<ShadowedElementAdapter> previousSibling(
      css::SelectorTraversalBudget* budget = nullptr) const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).previousSibling();
    return findElementSibling(target, false, budget);
  }

  std::optional<ShadowedElementAdapter> nextSibling(
      css::SelectorTraversalBudget* budget = nullptr) const {
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).nextSibling();
    return findElementSibling(target, true, budget);
  }

  xml::XMLQualifiedName tagName() const {
    return registry_.get().get<donner::components::TreeComponent>(treeEntity_).tagName();
  }

  bool isEmptyForSelector(css::SelectorTraversalBudget* budget) const {
    return IsSelectorEmpty(registry_.get(), dataEntity_, budget);
  }

  bool isKnownType() const {
    return registry_.get().get<ElementTypeComponent>(dataEntity_).type() !=
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
  std::optional<ShadowedElementAdapter> findElementSibling(
      Entity target, bool forward, css::SelectorTraversalBudget* budget) const {
    for (; target != entt::null;
         target = forward
                      ? registry_.get().get<donner::components::TreeComponent>(target).nextSibling()
                      : registry_.get()
                            .get<donner::components::TreeComponent>(target)
                            .previousSibling()) {
      if (registry_.get().all_of<ElementTypeComponent>(resolveData(target))) {
        return create(target);
      }
      if (budget != nullptr && !budget->consume()) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  /// Resolve a tree entity to its data entity: shadow entities proxy their light entity.
  Entity resolveData(Entity treeEntity) const {
    const auto* shadowComponent = registry_.get().try_get<ShadowEntityComponent>(treeEntity);
    return shadowComponent ? shadowComponent->lightEntity : treeEntity;
  }

  ShadowedElementAdapter create(Entity newTreeEntity) const {
    return ShadowedElementAdapter(registry_.get(), newTreeEntity, resolveData(newTreeEntity));
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
                                                      const ShadowedElementAdapter& adapter,
                                                      css::SelectorTraversalBudget* budget) {
  css::SelectorMatchOptions<ShadowedElementAdapter> options;
  options.traversalBudget = budget;
  for (std::size_t i = 0; i < rule.selector.entries.size(); ++i) {
    if (budget != nullptr && budget->rejected()) {
      return std::nullopt;
    }
    const css::SelectorMatchResult match = rule.selector.entries[i].matches(adapter, options);
    if (budget != nullptr && budget->rejected()) {
      return std::nullopt;
    }
    if (match) {
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

void ApplyRuleDeclarations(const css::SelectorRule& rule, css::Specificity specificity,
                           PropertyRegistry& properties, StyleResourceBudget& styleBudget,
                           ParseWarningSink& warningSink) {
  for (const auto& declaration : rule.declarations) {
    if (!styleBudget.reserveDeclarationApplication(declaration.values.size(),
                                                   declaration.sourceByteSize)) {
      return;
    }
    if (auto error = properties.parseProperty(declaration, specificity)) {
      constexpr std::size_t kMaximumStyleDiagnosticBytes = 4096;
      if (error->reason.size() > kMaximumStyleDiagnosticBytes) {
        error->reason =
            RcString(std::string_view(error->reason).substr(0, kMaximumStyleDiagnosticBytes));
      }
      warningSink.add(std::move(*error));
    }
  }
}

}  // namespace

std::shared_ptr<AnimationContextualWorkBudget>
StyleResourceBudget::contextualAnimationWorkBudget() {
  if (!contextualAnimationWork_) {
    contextualAnimationWork_ = std::make_shared<AnimationContextualWorkBudget>();
  }
  return contextualAnimationWork_;
}

void StyleResourceBudget::resetContextualAnimationWork() {
  if (contextualAnimationWork_) {
    contextualAnimationWork_->reset();
  }
}

StyleResourceBudget::ActiveRegistryScope::ActiveRegistryScope(StyleResourceBudget& budget,
                                                              Registry& registry)
    : budget_(budget),
      registry_(&registry),
      inserted_(budget.activeRegistries_.insert(&registry).second) {
  if (!inserted_) {
    return;
  }
  boundContextualWork_ = budget_.contextualAnimationWorkBudget();
  auto* context = registry.ctx().find<AnimationContextualWorkContext>();
  hadContext_ = context != nullptr;
  if (context) {
    previousContext_ = std::exchange(context->budget, boundContextualWork_);
  } else {
    registry.ctx().emplace<AnimationContextualWorkContext>(
        AnimationContextualWorkContext{boundContextualWork_});
  }
  if (auto* animationBudget = registry.ctx().find<AnimationResourceBudget>()) {
    previousBudget_ = animationBudget->contextualWorkBudget();
    animationBudget->resetContextualWork();
    animationBudget->setContextualWorkBudget(boundContextualWork_);
  }
}

StyleResourceBudget::ActiveRegistryScope::~ActiveRegistryScope() {
  if (!inserted_) {
    return;
  }
  if (auto* context = registry_->ctx().find<AnimationContextualWorkContext>();
      context && context->budget == boundContextualWork_) {
    if (hadContext_) {
      context->budget = previousContext_;
    } else {
      registry_->ctx().erase<AnimationContextualWorkContext>();
    }
  }
  if (auto* animationBudget = registry_->ctx().find<AnimationResourceBudget>();
      animationBudget && animationBudget->contextualWorkBudget() == boundContextualWork_) {
    animationBudget->setContextualWorkBudget(previousBudget_);
  }
  budget_.activeRegistries_.erase(registry_);
}

const ComputedStyleComponent& StyleSystem::computeStyle(EntityHandle handle,
                                                        ParseWarningSink& warningSink) {
  auto& computedStyle = handle.get_or_emplace<ComputedStyleComponent>();
  computePropertiesInto(handle, computedStyle, warningSink);
  return computedStyle;
}

void StyleSystem::updateStyle(EntityHandle handle, std::string_view style) {
  auto& styleComponent = handle.get_or_emplace<StyleComponent>();
  StyleComponent candidate = styleComponent;
  if (!candidate.updateStyle(style)) {
    return;
  }

  // Merge the new style string with the existing style attribute.
  auto& attributes = handle.get_or_emplace<donner::components::AttributesComponent>();
  const std::optional<RcString> existingStyleValue =
      attributes.getAttribute(xml::XMLQualifiedNameRef("style"));
  const std::string_view existingStyle =
      existingStyleValue.has_value() ? std::string_view(*existingStyleValue) : std::string_view();

  css::parser::DeclarationListParser::SecurityStats existingStats;
  css::parser::DeclarationListParser::SecurityStats updateStats;
  const std::vector<css::Declaration> existingDeclarations =
      css::parser::DeclarationListParser::ParseOnlyDeclarations(existingStyle, &existingStats);
  const std::vector<css::Declaration> updateDeclarations =
      css::parser::DeclarationListParser::ParseOnlyDeclarations(style, &updateStats);
  if (existingStats.rejected || updateStats.rejected) {
    return;
  }
  const std::string mergedStyle =
      css::mergeStyleDeclarations(existingDeclarations, updateDeclarations);

  attributes.setAttribute(*handle.registry(), xml::XMLQualifiedName("style"),
                          RcString(mergedStyle));
  styleComponent = std::move(candidate);

  invalidateComputed(handle);
}

void StyleSystem::applyStylesheetRules(Registry& registry, Entity treeEntity, Entity dataEntity,
                                       PropertyRegistry& properties,
                                       StyleResourceBudget& styleBudget,
                                       ParseWarningSink& warningSink) {
  const ShadowedElementAdapter adapter(registry, treeEntity, dataEntity);
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    const auto& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
    if (!IsApplicableStylesheet(registry, stylesheet, stylesheetEntity, dataEntity)) {
      continue;
    }
    for (const css::SelectorRule& rule : stylesheet.stylesheet.rules()) {
      if (!styleBudget.reserveRuleElementMatch()) {
        return;
      }
      auto match = MatchSelectorRule(rule, adapter, &styleBudget.selectorTraversal());
      if (!match) {
        continue;
      }
      css::Specificity specificity = match->specificity;
      if (stylesheet.isUserAgentStylesheet) {
        specificity = specificity.toUserAgentSpecificity();
      }
      ApplyRuleDeclarations(rule, specificity, properties, styleBudget, warningSink);
    }
  }
}

ResolvedPaintServer StyleSystem::externalHostPaint(Registry& registry, bool stroke) {
  const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
  if (!occurrence) {
    return PaintServer::None();
  }
  return stroke ? occurrence->stroke : occurrence->fill;
}

void StyleSystem::rejectExternalHostResource(Registry& registry) {
  if (auto* occurrence = registry.ctx().find<ExternalUseStyleContext>()) {
    occurrence->unsupportedResource = true;
  }
}

std::optional<PaintServer> StyleSystem::localPaintForInstantiation(
    const ComputedStyleComponent& style, bool stroke) {
  if (stroke ? style.externalStroke : style.externalFill) {
    return std::nullopt;
  }
  return stroke ? style.properties->stroke.get() : style.properties->fill.get();
}

PropertyRegistry StyleSystem::inheritComputedParent(EntityHandle handle, Entity parent,
                                                    PropertyRegistry properties,
                                                    ComputedStyleComponent& computed,
                                                    ParseWarningSink& warningSink) {
  Registry& registry = *handle.registry();
  const auto* occurrence = ExternalRootContext(registry, handle.entity());
  const PropertyState fillState = properties.fill.state;
  const PropertyState strokeState = properties.stroke.state;
  PropertyRegistry result;
  if (occurrence) {
    result = inheritExternalParent(properties, *occurrence->parent.properties);
    if (BlockExternalResources(result, properties)) {
      rejectExternalHostResource(registry);
      warningSink.add(ParseDiagnostic::Error("External-use inherited URL resources are unsupported",
                                             FileOffset::Offset(0)));
    }
  } else {
    result = inheritProperties(registry, parent, std::move(properties), warningSink);
  }
  const auto* parentStyle =
      parent != entt::null ? registry.try_get<ComputedStyleComponent>(parent) : nullptr;
  const bool allowPaint = occurrence || !registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
  computed.externalFill =
      ExternalPaintOrigin(fillState, occurrence != nullptr, parentStyle, false, allowPaint);
  computed.externalStroke =
      ExternalPaintOrigin(strokeState, occurrence != nullptr, parentStyle, true, allowPaint);
  return result;
}

void StyleSystem::resolveComputedParentFonts(EntityHandle handle, Entity parent,
                                             PropertyRegistry& properties) {
  if (const auto* occurrence = ExternalRootContext(*handle.registry(), handle.entity())) {
    const PropertyRegistry& host = *occurrence->parent.properties;
    properties.resolveFontSize(host.fontSize.get().value().value);
    properties.resolveFontWeight(host.fontWeight.get().value());
    properties.resolveFontStretch(host.fontStretch.get().value());
  } else {
    resolveRelativeFontProperties(*handle.registry(), parent, properties);
  }
}

PropertyRegistry StyleSystem::inheritExternalParent(const PropertyRegistry& local,
                                                    const PropertyRegistry& parent) {
  PropertyRegistry result = local;
  auto destination = result.allPropertiesMutable();
  const auto source = local.allProperties();
  const auto inherited = parent.allProperties();
  [&]<std::size_t... index>(std::index_sequence<index...>) {
    ((std::get<index>(destination) =
          InheritExternalProperty(std::get<index>(source), std::get<index>(inherited))),
     ...);
  }(std::make_index_sequence<PropertyRegistry::numProperties()>{});
  return result;
}

PropertyRegistry StyleSystem::inheritProperties(Registry& registry, Entity parent,
                                                PropertyRegistry properties,
                                                ParseWarningSink& warningSink) {
  if (parent == entt::null) {
    return properties;
  }
  auto& parentStyleComponent = registry.get_or_emplace<ComputedStyleComponent>(parent);
  computePropertiesInto(EntityHandle(registry, parent), parentStyleComponent, warningSink);
  const PropertyInheritOptions inheritOptions = registry.all_of<DoNotInheritFillOrStrokeTag>(parent)
                                                    ? PropertyInheritOptions::NoPaint
                                                    : PropertyInheritOptions::All;
  PropertyRegistry inherited =
      properties.inheritFrom(parentStyleComponent.properties.value(), inheritOptions);
  PreserveLocalValue(properties.fontSize, inherited.fontSize);
  PreserveLocalValue(properties.color, inherited.color);
  PreserveLocalValue(properties.fill, inherited.fill);
  PreserveLocalValue(properties.stroke, inherited.stroke);
  PreserveLocalValue(properties.colorInterpolation, inherited.colorInterpolation);
  PreserveLocalValue(properties.opacity, inherited.opacity);
  PreserveLocalValue(properties.fillOpacity, inherited.fillOpacity);
  PreserveLocalValue(properties.strokeOpacity, inherited.strokeOpacity);
  PreserveLocalValue(properties.strokeMiterlimit, inherited.strokeMiterlimit);
  return inherited;
}

void StyleSystem::resolveRelativeFontProperties(Registry& registry, Entity parent,
                                                PropertyRegistry& properties) {
  double parentFontSizePx = 12.0;
  int parentFontWeight = 400;
  int parentFontStretch = static_cast<int>(FontStretch::Normal);
  if (parent != entt::null) {
    const auto& parentStyle = registry.get<ComputedStyleComponent>(parent);
    if (parentStyle.properties) {
      parentFontSizePx = parentStyle.properties->fontSize.get().value().value;
      parentFontWeight = parentStyle.properties->fontWeight.get().value();
      parentFontStretch = parentStyle.properties->fontStretch.get().value();
    }
  }
  properties.resolveFontSize(parentFontSizePx);
  properties.resolveFontWeight(parentFontWeight);
  properties.resolveFontStretch(parentFontStretch);
}

void StyleSystem::computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                                        ParseWarningSink& warningSink) {
  if (computedStyle.properties) {
    return;  // Already computed.
  }

  Registry& registry = *handle.registry();
  auto& styleBudget = GetStyleResourceBudget(registry);

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();

  // Apply local style.
  PropertyRegistry properties;
  if (auto* styleComponent = registry.try_get<StyleComponent>(dataEntity)) {
    properties = styleComponent->properties;
  } else {
    properties = PropertyRegistry();
  }

  applyStylesheetRules(registry, handle.entity(), dataEntity, properties, styleBudget, warningSink);
  ApplyAnimatedStyle(registry, dataEntity, properties);
  const Entity parent = handle.get<donner::components::TreeComponent>().parent();
  const auto* occurrence = ExternalRootContext(registry, handle.entity());
  const ComputedStyleComponent* parentStyle =
      occurrence             ? &occurrence->parent
      : parent == entt::null ? nullptr
                             : &computeStyle(EntityHandle(registry, parent), warningSink);
  ApplyContextualFontSize(registry, dataEntity, parentStyle, properties);
  const auto resourceOpacities = ApplyScalarAnimationStyles(registry, dataEntity, parentStyle,
                                                            properties, styleBudget, warningSink);
  ApplySymbolInstanceDisplay(handle, dataEntity, properties);
  properties.resolveElementColors(warningSink);
  const bool inheritPaint =
      occurrence || parent == entt::null || !registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
  ApplyPaintAnimationStyles(registry, dataEntity, parentStyle, inheritPaint, properties);

  // Inherit from parent, then charge the dynamic bytes retained by the final computed style.
  // Charging the parent's source value alone misses large locally parsed vectors and can also
  // differ from the actual value selected by the cascade.
  PropertyRegistry finalProperties =
      inheritComputedParent(handle, parent, std::move(properties), computedStyle, warningSink);

  if (styleBudget.reserveComplexPropertyBytes(handle.entity(),
                                              finalProperties.complexPropertyBytes())) {
    computedStyle.properties = std::move(finalProperties);
    computedStyle.resourceOpacities = resourceOpacities;
  } else {
    // Once the aggregate retained-byte budget is exhausted, use bounded initial styles for the
    // remainder of the pass instead of retaining attacker-sized local or inherited vectors.
    styleBudget.release(handle.entity());
    computedStyle.externalFill = false;
    computedStyle.externalStroke = false;
    computedStyle.properties = PropertyRegistry();
    computedStyle.resourceOpacities = ComputedResourceOpacities();
  }

  // Resolve font-size to absolute pixels. CSS spec requires the computed value of font-size to
  // always be an absolute length. Relative units (em, %, ex) resolve against the parent's computed
  // font-size, and percentages resolve against the parent font-size (not the viewBox).
  if (computedStyle.properties) {
    resolveComputedParentFonts(handle, parent, *computedStyle.properties);
    const double rootFontSizePx = ComputedRootFontSize(registry);
    computedStyle.properties->resolveTextDecorationThickness(rootFontSizePx);
    computedStyle.properties->resolveInsetClipLengths(rootFontSizePx);
  }
}

std::vector<MatchedStyleRule> StyleSystem::collectMatchedStyleRules(EntityHandle handle) const {
  Registry& registry = *handle.registry();

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();

  std::vector<MatchedStyleRule> result;
  StyleResourceBudget styleBudget;
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    const auto& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
    if (!IsApplicableStylesheet(registry, stylesheet, stylesheetEntity, dataEntity)) {
      continue;
    }
    const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();

    for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
      if (!styleBudget.reserveRuleElementMatch()) {
        break;
      }
      const css::SelectorRule& rule = rules[ruleIndex];
      std::optional<MatchedSelectorEntry> match =
          MatchSelectorRule(rule, ShadowedElementAdapter(registry, handle.entity(), dataEntity),
                            &styleBudget.selectorTraversal());
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
    if (!IsActiveAuthorStylesheet(registry, stylesheetEntity, stylesheet)) {
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
  auto& styleBudget = GetStyleResourceBudget(registry);
  styleBudget.reset();
  const auto* renderState = registry.ctx().find<RenderTreeState>();
  const bool hasBeenBuilt = renderState != nullptr && renderState->hasBeenBuilt;
  const bool needsFullStyleRecompute =
      renderState != nullptr && renderState->needsFullStyleRecompute;
  const bool hasDirtyEntities = !registry.view<DirtyFlagsComponent>().empty();

  // Selective pass: recompute only the entities flagged dirty. This depends on a two-part
  // invariant that mutation hooks must uphold together, because either half alone is inert:
  //
  //   1. The entity's cached ComputedStyleComponent must have been removed. computeStyle() is
  //      memoized (see computePropertiesInto, which returns early once `properties` is set), so
  //      a Style dirty flag on an entity whose cached style is still populated does nothing.
  //   2. The entity must be flagged DirtyFlagsComponent::Style. Removing the cached style without
  //      flagging it leaves the entity with no computed style at all, which later passes that
  //      walk ComputedStyleComponent would then skip or dereference.
  //
  // Mutations whose effect is not local to specific entities (stylesheet edits, class/id/attribute
  // writes that feed selectors, structural changes) cannot express themselves through per-entity
  // flags at all, and must instead request RenderTreeState::needsFullStyleRecompute.
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
      SynchronizeStylesheetFonts(resourceManager, registry, stylesheetEntity, stylesheet);
    }

    return;
  }

  if (hasBeenBuilt && needsFullStyleRecompute) {
    styleBudget.releaseAll();
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
    SynchronizeStylesheetFonts(resourceManager, registry, stylesheetEntity, stylesheet);
  }
}

void StyleSystem::computeStylesFor(Registry& registry, std::span<const Entity> entities,
                                   ParseWarningSink& warningSink) {
  for (Entity entity : entities) {
    computeStyle(EntityHandle(registry, entity), warningSink);
  }
}

bool StyleSystem::anyStylesheetUsesAttributeInSelector(Registry& registry,
                                                       std::string_view localName) const {
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    if (!HasSvgNamespace(registry, stylesheetEntity)) {
      continue;
    }
    if (view.get<StylesheetComponent>(stylesheetEntity).usesAttributeInSelector(localName)) {
      return true;
    }
  }

  return false;
}

void StyleSystem::invalidateComputed(EntityHandle handle) {
  if (auto* budget = handle.registry()->ctx().find<StyleResourceBudget>()) {
    budget->release(handle.entity());
  }
  handle.remove<ComputedStyleComponent>();
}

void StyleSystem::invalidateAll(EntityHandle handle) {
  invalidateComputed(handle);
  // TODO: Reparse attributes.
}

}  // namespace donner::svg::components
