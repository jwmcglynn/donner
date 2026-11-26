#include "donner/svg/components/style/StyleSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/StringUtils.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLNodeTypeComponent.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLValueComponent.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/css/VariableGraph.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/CustomPropertyMap.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/components/style/DocumentLayerOrder.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/PaintAnimationStyle.h"
#include "donner/svg/components/style/RuleIndex.h"
#include "donner/svg/components/style/ScalarAnimationStyle.h"
#include "donner/svg/components/style/SelectorContent.h"
#include "donner/svg/components/style/StyleAncestorFilter.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/StyleElementView.h"
#include "donner/svg/components/style/StylePassScheduler.h"
#include "donner/svg/components/text/TextComponent.h"

namespace donner::svg::components {

namespace {

/**
 * Root computed font size for rem lengths resolved before descendants inherit them.
 * @param registry Registry containing the document.
 * @param entity Entity whose style is being computed.
 * @param properties Its computed properties with resolved fonts, used when it is the root.
 */
double ComputedRootFontSize(const Registry& registry, Entity entity,
                            const PropertyRegistry& properties) {
  const auto* context = registry.ctx().find<SVGDocumentContext>();
  if (context && registry.valid(context->rootEntity)) {
    if (context->rootEntity == entity) {
      return properties.fontSize.get().value().value;
    }
    // Installed root properties avoid re-entering style computation for root-owned rem.
    if (const auto* root = registry.try_get<ComputedStyleComponent>(context->rootEntity);
        root && root->properties()) {
      return root->properties()->fontSize.get().value().value;
    }
  }
  return FontMetrics().rootFontSize;
}

void SynchronizeStylesheetFonts(ResourceManagerContext& resources, Registry& registry,
                                Entity entity, const StylesheetComponent& stylesheet) {
  const css::Stylesheet empty;
  resources.synchronizeStylesheetFontFaces(
      entity, HasSvgNamespace(registry, entity) ? stylesheet.cascadeFontFaces() : empty.fontFaces(),
      stylesheet.fontFaceRevision);
}

bool IsActiveAuthorStylesheet(const Registry& registry, Entity entity,
                              const StylesheetComponent& stylesheet) {
  return HasSvgNamespace(registry, entity) && stylesheet.isCssType() &&
         !stylesheet.isUserAgentStylesheet && !stylesheet.sourceMap.empty();
}

css::Specificity CascadeSpecificity(css::Specificity specificity, bool userAgent) {
  return userAgent ? specificity.toUserAgentSpecificity() : specificity;
}

bool IsApplicableStylesheet(const Registry& registry, const StylesheetComponent& stylesheet,
                            Entity stylesheetEntity, Entity dataEntity) {
  if (!HasSvgNamespace(registry, stylesheetEntity) || !stylesheet.isCssType()) {
    return false;
  }
  if (!stylesheet.isUserAgentStylesheet) {
    return true;
  }
  // The built-in sheet has an SVG default namespace; authored namespace handling is separate.
  const auto* type = registry.try_get<ElementTypeComponent>(dataEntity);
  return type && type->inSvgNamespace();
}

template <typename Visitor>
void VisitCascadeStylesheets(const Registry& registry, const DocumentLayerOrder* layers,
                             Visitor visit) {
  const auto view = registry.view<const StylesheetComponent>();
  const auto visitRange = [&](const auto& range) {
    for (Entity entity : range) {
      if (!visit(view.get<const StylesheetComponent>(entity), entity)) {
        break;
      }
    }
  };
  if (layers) {
    visitRange(layers->stylesheets());
  } else {
    visitRange(view);
  }
}

class RuleLayerRanks {
public:
  RuleLayerRanks(const DocumentLayerOrder* layers, bool userAgent, const css::Stylesheet& sheet)
      : ranks_(layers && layers->hasRanks() && !userAgent ? std::optional(layers->ranksFor(sheet))
                                                          : std::nullopt) {}

  std::optional<std::uint16_t> rank(css::LayerId layer) const {
    if (!ranks_) {
      return layer == css::kUnlayered ? std::optional<std::uint16_t>(0) : std::nullopt;
    }
    const auto rank = ranks_->forLayer(layer);
    return rank == 0 ? std::nullopt : std::optional(rank);
  }

private:
  std::optional<DocumentLayerOrder::SheetRanks> ranks_;
};

template <typename Visitor>
bool VisitCascadeImports(const StylesheetComponent& stylesheet, const DocumentLayerOrder* layers,
                         Visitor visit) {
  if (layers && !stylesheet.importedStylesheets.empty() &&
      !layers->allowsImportsOf(stylesheet.stylesheet)) {
    return true;
  }
  // Imported rules precede the importing sheet's own rules in the cascade.
  UTILS_RELEASE_ASSERT(stylesheet.importedRuleIndexes.size() ==
                       stylesheet.importedStylesheets.size());
  for (std::size_t i = 0; i < stylesheet.importedStylesheets.size(); ++i) {
    if (!visit(stylesheet.contentRevision(), i, stylesheet.importedStylesheets[i],
               stylesheet.importedRuleIndexes[i], stylesheet.isUserAgentStylesheet)) {
      return false;
    }
  }
  return true;
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

StyleCascadeMetrics& GetStyleCascadeMetrics(Registry& registry) {
  if (auto* metrics = registry.ctx().find<StyleCascadeMetrics>()) {
    return *metrics;
  }
  return registry.ctx().emplace<StyleCascadeMetrics>();
}

MatchedPropertiesCache& GetMatchedPropertiesCache(Registry& registry) {
  if (auto* cache = registry.ctx().find<MatchedPropertiesCache>()) {
    return *cache;
  }
  return registry.ctx().emplace<MatchedPropertiesCache>();
}

MatchedOpacityInput OpacityInput(const Property<double>& property) {
  return MatchedOpacityInput{
      .state = property.state,
      .value = property.get(),
      .specificity = property.specificity,
  };
}

MatchedOpacityInput StopOpacityInput(const Registry& registry, Entity dataEntity) {
  static const StopProperties defaults;
  if (const auto* stop = registry.try_get<StopComponent>(dataEntity)) {
    return OpacityInput(stop->properties.stopOpacity);
  }
  return OpacityInput(defaults.stopOpacity);
}

MatchedOpacityInput FloodOpacityInput(const Registry& registry, Entity dataEntity) {
  static const FEFloodComponent defaults;
  if (const auto* flood = registry.try_get<FEFloodComponent>(dataEntity)) {
    return OpacityInput(flood->floodOpacity);
  }
  if (const auto* shadow = registry.try_get<FEDropShadowComponent>(dataEntity)) {
    return OpacityInput(shadow->floodOpacity);
  }
  return OpacityInput(defaults.floodOpacity);
}

std::array<const css::Declaration*, 2> ResourceOpacityDeclarations(
    const PropertyRegistry& properties) {
  std::array<const css::Declaration*, 2> result{};
  if (const auto stop = properties.unparsedProperties.find(RcString("stop-opacity"));
      stop != properties.unparsedProperties.end()) {
    result[0] = &stop->second.declaration;
  }
  if (const auto flood = properties.unparsedProperties.find(RcString("flood-opacity"));
      flood != properties.unparsedProperties.end()) {
    result[1] = &flood->second.declaration;
  }
  return result;
}

bool ChargeCachedResourceOpacityWork(const PropertyRegistry& properties,
                                     StyleResourceBudget& budget, StyleCascadeCounters& counters) {
  const auto declarations = ResourceOpacityDeclarations(properties);
  std::size_t applications = 0;
  std::size_t componentWork = 0;
  std::size_t byteWork = 0;
  for (const css::Declaration* declaration : declarations) {
    if (declaration) {
      ++applications;
      componentWork += declaration->values.size();
      byteWork += declaration->sourceByteSize;
    }
  }
  if (!budget.canReserveDeclarationWork(applications, componentWork, byteWork)) {
    return false;
  }
  for (const css::Declaration* declaration : declarations) {
    if (!declaration) {
      continue;
    }
    const bool admitted = budget.reserveDeclarationApplication(declaration->values.size(),
                                                               declaration->sourceByteSize);
    UTILS_RELEASE_ASSERT(admitted);
    ++counters.declarationParsesSkipped;
  }
  return true;
}

/// Adds the duration of the outermost timed style call on a registry to its style time.
class StyleTimeScope {
public:
  explicit StyleTimeScope(StyleCascadeMetrics& metrics)
      : metrics_(metrics), outermost_(metrics.activeTimedCalls++ == 0) {
    if (outermost_) {
      start_ = std::chrono::steady_clock::now();
    }
  }
  ~StyleTimeScope() {
    --metrics_.activeTimedCalls;
    if (outermost_) {
      metrics_.styleTime += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start_);
    }
  }
  StyleTimeScope(const StyleTimeScope&) = delete;
  StyleTimeScope& operator=(const StyleTimeScope&) = delete;

private:
  StyleCascadeMetrics& metrics_;
  bool outermost_;
  std::chrono::steady_clock::time_point start_;
};

/// Adds the whole-registry copies of the outermost element cascade on a registry to its counters;
/// a parent cascaded on demand inside it is counted with it.
class RegistryCopyCount {
public:
  explicit RegistryCopyCount(StyleSystem::CascadePass& pass)
      : pass_(pass),
        outermost_(pass.activeCascades++ == 0),
        start_(outermost_ ? details::PropertyRegistryCopyCount() : 0) {}
  ~RegistryCopyCount() {
    --pass_.activeCascades;
    if (outermost_) {
      pass_.counters.registryCopies += details::PropertyRegistryCopyCount() - start_;
    }
  }
  RegistryCopyCount(const RegistryCopyCount&) = delete;
  RegistryCopyCount& operator=(const RegistryCopyCount&) = delete;

private:
  StyleSystem::CascadePass& pass_;
  bool outermost_;
  std::uint64_t start_;
};

/// Counts a style pass that ends, from any return path, with its budget refusing work.
class PassRejectionCount {
public:
  PassRejectionCount(const StyleResourceBudget& budget, StyleCascadeCounters& counters)
      : budget_(budget), counters_(counters) {}
  ~PassRejectionCount() {
    if (budget_.rejected()) {
      ++counters_.budgetRejections;
    }
  }
  PassRejectionCount(const PassRejectionCount&) = delete;
  PassRejectionCount& operator=(const PassRejectionCount&) = delete;

private:
  const StyleResourceBudget& budget_;
  StyleCascadeCounters& counters_;
};

/// The ancestor filter of one style pass, or none while the registry holds
/// \ref UnfilteredCascadeForTesting. Adds the filter's disables to the counters when the pass
/// ends, from any return path.
class PassAncestorFilter {
public:
  PassAncestorFilter(const Registry& registry, StyleCascadeCounters& counters, bool enabled)
      : counters_(counters) {
    if (enabled && !registry.ctx().contains<UnfilteredCascadeForTesting>() &&
        !registry.ctx().contains<CascadeAncestorFilters>()) {
      filter_.emplace(registry);
    }
  }
  ~PassAncestorFilter() {
    if (filter_) {
      counters_.ancestorFilterSaturations += filter_->saturationDisables();
      counters_.ancestorFilterKeyLimits += filter_->keyLimitDisables();
    }
  }
  PassAncestorFilter(const PassAncestorFilter&) = delete;
  PassAncestorFilter& operator=(const PassAncestorFilter&) = delete;

  /// The filter, or null for an unfiltered pass.
  StyleAncestorFilter* get() { return filter_ ? &*filter_ : nullptr; }

private:
  StyleCascadeCounters& counters_;
  std::optional<StyleAncestorFilter> filter_;
};

/// Consults a pass's ancestor filter about the candidate rules of one element and counts its
/// answers. The filter is synchronized to the element on the first candidate with ancestor keys,
/// so an element without one costs no synchronization.
class AncestorFilterCheck {
public:
  AncestorFilterCheck(StyleAncestorFilter* filter, Entity treeEntity,
                      StyleCascadeCounters& counters)
      : filter_(filter), treeEntity_(treeEntity), counters_(counters) {}

  /// True, counting a rejection, if every selector-list entry of the rule at \p position needs
  /// an ancestor key no ancestor has, so the rule cannot match.
  bool rejects(const RuleIndex& index, std::uint32_t position) {
    groups_ = filter_ ? index.ancestorHashes(position) : Groups();
    if (groups_.empty() || groups_.size() > remainingGroups_) {
      groups_ = Groups();
      return false;
    }
    if (!synchronized_) {
      filter_->synchronizeTo(treeEntity_);
      synchronized_ = true;
    }
    if (!filter_->enabled()) {
      groups_ = Groups();
      return false;
    }
    remainingGroups_ -= groups_.size();
    const bool rejected =
        std::ranges::all_of(groups_, [&](const RuleIndex::AncestorHashGroup& group) {
          return std::ranges::any_of(
              group, [&](std::uint32_t hash) { return !filter_->mayContain(BloomHash(hash)); });
        });
    counters_.ancestorFilterRejections += rejected ? 1 : 0;
    return rejected;
  }

  /// Use the selector matcher only for a candidate the filter cannot rule out.
  template <typename Match>
  auto evaluate(const RuleIndex& index, std::uint32_t position, const Match& match) {
    if (rejects(index, position)) {
      return decltype(match()){};
    }
    auto result = match();
    if (!result && !groups_.empty()) {
      ++counters_.ancestorFilterFalsePositives;
    }
    return result;
  }

  bool synchronized() const { return synchronized_; }

private:
  using Groups = std::span<const RuleIndex::AncestorHashGroup>;
  // Bound probes across all candidates of an element, independent of selector-list length.
  static constexpr std::size_t kMaximumGroupsPerElement = 256;

  StyleAncestorFilter* filter_;
  Entity treeEntity_;
  StyleCascadeCounters& counters_;
  Groups groups_;
  std::size_t remainingGroups_ = kMaximumGroupsPerElement;
  bool synchronized_ = false;
};

bool InheritsExternalPaint(PropertyState state) {
  return state == PropertyState::NotSet || state == PropertyState::Inherit ||
         state == PropertyState::ExplicitUnset;
}

/// Inherit an external-use parent's value in place when the local declaration defers to it.
template <typename T, PropertyCascade cascade>
void InheritExternalProperty(Property<T, cascade>& property, const Property<T, cascade>& parent) {
  const bool inherited = cascade != PropertyCascade::None;
  if (property.state != PropertyState::Inherit &&
      !(inherited && InheritsExternalPaint(property.state))) {
    return;
  }
  property.value = parent.get();
  property.state = PropertyState::Set;
  property.specificity = css::Specificity().toUserAgentSpecificity();
}

/// Local declaration states that decide whether an inherited external-use resource is refused,
/// read before inheritance replaces them.
struct LocalResourceStates {
  explicit LocalResourceStates(const PropertyRegistry& local)
      : markerStart(local.markerStart.state),
        markerMid(local.markerMid.state),
        markerEnd(local.markerEnd.state),
        mask(local.mask.state),
        clipPath(local.clipPath.state),
        filter(local.filter.state),
        cursor(local.cursor.state) {}

  PropertyState markerStart;
  PropertyState markerMid;
  PropertyState markerEnd;
  PropertyState mask;
  PropertyState clipPath;
  PropertyState filter;
  PropertyState cursor;
};

template <PropertyCascade cascade>
bool BlockExternalReference(Property<Reference, cascade>& property, PropertyState local) {
  const bool inherits = cascade == PropertyCascade::None ? local == PropertyState::Inherit
                                                         : InheritsExternalPaint(local);
  const auto reference = property.get();
  if (!inherits || !reference || reference->href.empty()) {
    return false;
  }
  property.set(PropertyState::ExplicitInitial, css::Specificity());
  return true;
}

bool BlockExternalClipPath(PropertyRegistry& properties, PropertyState local) {
  const ClipPath* clip = properties.clipPath.getStoredValue();
  if (local != PropertyState::Inherit || !clip) {
    return false;
  }
  const auto* reference = std::get_if<Reference>(&clip->value);
  if (!reference || reference->href.empty()) {
    return false;
  }
  properties.clipPath.set(PropertyState::ExplicitInitial, css::Specificity());
  return true;
}

bool BlockExternalResources(PropertyRegistry& properties, const LocalResourceStates& local) {
  bool rejected = BlockExternalReference(properties.markerStart, local.markerStart);
  rejected |= BlockExternalReference(properties.markerMid, local.markerMid);
  rejected |= BlockExternalReference(properties.markerEnd, local.markerEnd);
  rejected |= BlockExternalReference(properties.mask, local.mask);
  rejected |= BlockExternalClipPath(properties, local.clipPath);
  if (local.filter == PropertyState::Inherit) {
    const auto* effects = properties.filter.getStoredValue();
    if (effects && std::any_of(effects->begin(), effects->end(), [](const FilterEffect& effect) {
          return effect.is<FilterEffect::ElementReference>();
        })) {
      properties.filter.set(PropertyState::ExplicitInitial, css::Specificity());
      rejected = true;
    }
  }
  if (InheritsExternalPaint(local.cursor) && !properties.cursor.get().value().images.empty()) {
    properties.cursor.set(PropertyState::ExplicitInitial, css::Specificity());
    rejected = true;
  }
  return rejected;
}

const ExternalUseStyleContext* ExternalRootContext(const Registry& registry, Entity entity) {
  const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
  return occurrence && occurrence->root == entity ? occurrence : nullptr;
}

bool IsDirectUseInstanceRoot(entt::basic_handle<const Registry> handle) {
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

void ApplySymbolInstanceDisplay(entt::basic_handle<const Registry> handle, Entity dataEntity,
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

void ApplyAnimatedStyle(const Registry& registry, Entity dataEntity, PropertyRegistry& properties) {
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
    case PropertyState::PendingSubstitution:
    case PropertyState::Inherit:
    case PropertyState::ExplicitUnset: return Lengthd(parentSize, Lengthd::Unit::Px);
    default:
      return ResolveFontOperand(properties.fontSize.get().value().toRcString().str(), parentSize)
          .value_or(Lengthd(12, Lengthd::Unit::Px));
  }
}

void ApplyContextualFontSize(const Registry& registry, AnimationResourceBudget* animationBudget,
                             Entity dataEntity, const ComputedStyleComponent* parentStyle,
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
  UTILS_RELEASE_ASSERT(animationBudget);
  if (!animationBudget->reserveContextualWork(recipe->second->evaluationWork)) {
    return;
  }
  double parentSize = 12;
  if (parentStyle && parentStyle->properties()) {
    parentSize = parentStyle->properties()->fontSize.get().value().value;
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

/// Properties whose set or explicitly initial local value is kept when the parent's declaration
/// is more specific.
constexpr auto kPreservedLocalProperties = std::make_tuple(
    &PropertyRegistry::fontSize, &PropertyRegistry::textIndent, &PropertyRegistry::color,
    &PropertyRegistry::fill, &PropertyRegistry::stroke, &PropertyRegistry::colorInterpolation,
    &PropertyRegistry::opacity, &PropertyRegistry::fillOpacity, &PropertyRegistry::strokeOpacity,
    &PropertyRegistry::strokeMiterlimit);

template <typename PropertyType>
std::optional<PropertyType> PreservedLocalValue(const PropertyType& local) {
  if (local.state == PropertyState::Set || local.state == PropertyState::ExplicitInitial) {
    return local;
  }
  return std::nullopt;
}

/// Inherit \p parent into \p properties in place, keeping the preserved local values.
void InheritPreservingLocalValues(PropertyRegistry& properties, const PropertyRegistry& parent,
                                  PropertyInheritOptions options) {
  std::apply(
      [&](auto... members) {
        auto preserved = std::make_tuple(PreservedLocalValue(properties.*members)...);
        properties.inherit(parent, options);
        std::apply(
            [&](auto&... values) {
              ((values ? void(properties.*members = std::move(*values)) : void()), ...);
            },
            preserved);
      },
      kPreservedLocalProperties);
}

struct ShadowedElementAdapter {
  ShadowedElementAdapter(const Registry& registry, Entity treeEntity, Entity dataEntity,
                         bool* subjectOnly = nullptr, bool* tagOnly = nullptr)
      : registry_(registry),
        treeEntity_(treeEntity),
        dataEntity_(dataEntity),
        subjectOnly_(subjectOnly),
        tagOnly_(tagOnly) {}

  bool operator==(const ShadowedElementAdapter& other) const {
    observeOccurrence();
    return treeEntity_ == other.treeEntity_;
  }

  Entity entity() const { return treeEntity_; }

  std::optional<ShadowedElementAdapter> parentElement() const {
    observeOccurrence();
    // A parentless element matches `:root`; see StyleParentElement for where the walk stops.
    const Entity target = StyleParentElement(registry_.get(), treeEntity_);
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> firstChild(
      css::SelectorTraversalBudget* budget = nullptr) const {
    observeOccurrence();
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).firstChild();
    return findElementSibling(target, true, budget);
  }

  std::optional<ShadowedElementAdapter> lastChild(
      css::SelectorTraversalBudget* budget = nullptr) const {
    observeOccurrence();
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).lastChild();
    return findElementSibling(target, false, budget);
  }

  std::optional<ShadowedElementAdapter> previousSibling(
      css::SelectorTraversalBudget* budget = nullptr) const {
    observeOccurrence();
    if (isShadowTreeRoot(treeEntity_)) {
      return std::nullopt;
    }
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).previousSibling();
    return findElementSibling(target, false, budget);
  }

  std::optional<ShadowedElementAdapter> nextSibling(
      css::SelectorTraversalBudget* budget = nullptr) const {
    observeOccurrence();
    if (isShadowTreeRoot(treeEntity_)) {
      return std::nullopt;
    }
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).nextSibling();
    return findElementSibling(target, true, budget);
  }

  xml::XMLQualifiedName tagName() const {
    return registry_.get().get<donner::components::TreeComponent>(treeEntity_).tagName();
  }

  bool isEmptyForSelector(css::SelectorTraversalBudget* budget) const {
    observeOccurrence();
    return IsSelectorEmpty(registry_.get(), dataEntity_, budget);
  }

  bool isKnownType() const {
    return registry_.get().get<ElementTypeComponent>(dataEntity_).type() !=
           svg::ElementType::Unknown;
  }

  RcString id() const {
    observeOtherKey();
    if (const auto* component = registry_.get().try_get<IdComponent>(dataEntity_)) {
      return component->id();
    } else {
      return "";
    }
  }

  RcString className() const {
    observeOtherKey();
    if (const auto* component = registry_.get().try_get<ClassComponent>(dataEntity_)) {
      return component->className;
    } else {
      return "";
    }
  }

  bool hasAttribute(const xml::XMLQualifiedNameRef& name) const {
    observeOtherKey();
    if (const auto* component =
            registry_.get().try_get<donner::components::AttributesComponent>(dataEntity_)) {
      return component->hasAttribute(name);
    } else {
      return false;
    }
  }

  std::optional<RcString> getAttribute(const xml::XMLQualifiedNameRef& name) const {
    observeOtherKey();
    if (const auto* component =
            registry_.get().try_get<donner::components::AttributesComponent>(dataEntity_)) {
      return component->getAttribute(name);
    } else {
      return std::nullopt;
    }
  }

  SmallVector<xml::XMLQualifiedNameRef, 1> findMatchingAttributes(
      const xml::XMLQualifiedNameRef& matcher) const {
    observeOtherKey();
    if (const auto* component =
            registry_.get().try_get<donner::components::AttributesComponent>(dataEntity_)) {
      return component->findMatchingAttributes(matcher);
    } else {
      return {};
    }
  }

private:
  void observeOtherKey() const {
    if (tagOnly_) {
      *tagOnly_ = false;
    }
  }
  void observeOccurrence() const {
    observeOtherKey();
    if (subjectOnly_) {
      *subjectOnly_ = false;
    }
  }

  /**
   * Whether \p treeEntity is the root of an instantiated shadow tree. The main shadow root of a
   * `<use>` is attached under its host while a frame is built, but instanced content is not part
   * of the host's tree: a shadow root is never a child or sibling of the elements around it, and
   * has no siblings of its own, so selectors match the same whether or not it is attached.
   */
  bool isShadowTreeRoot(Entity treeEntity) const {
    return registry_.get().all_of<ShadowTreeRootComponent>(treeEntity);
  }

  /// The first element at or after \p target in the given direction, skipping shadow roots.
  std::optional<ShadowedElementAdapter> findElementSibling(
      Entity target, bool forward, css::SelectorTraversalBudget* budget) const {
    for (; target != entt::null;
         target = forward
                      ? registry_.get().get<donner::components::TreeComponent>(target).nextSibling()
                      : registry_.get()
                            .get<donner::components::TreeComponent>(target)
                            .previousSibling()) {
      if (!isShadowTreeRoot(target) &&
          registry_.get().all_of<ElementTypeComponent>(resolveData(target))) {
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
    return StyleDataEntity(registry_.get(), treeEntity);
  }

  ShadowedElementAdapter create(Entity newTreeEntity) const {
    return ShadowedElementAdapter(registry_.get(), newTreeEntity, resolveData(newTreeEntity),
                                  subjectOnly_, tagOnly_);
  }

  std::reference_wrapper<const Registry> registry_;
  Entity treeEntity_;
  Entity dataEntity_;
  bool* subjectOnly_;
  bool* tagOnly_;
};

/**
 * Replace \p candidates with the positions of the rules of \p rules to test against an element:
 * the index's candidates, or every rule when the unindexed cascade is requested for testing.
 */
void CollectCandidateRules(std::span<const css::SelectorRule> rules, const RuleIndex& index,
                           const RuleIndexElementView& element, bool unindexed,
                           std::vector<std::uint32_t>& candidates) {
  UTILS_RELEASE_ASSERT_MSG(index.ruleCount() == rules.size(),
                           "Rule index is stale: rebuild it when its stylesheet changes");
  if (unindexed) {
    candidates.resize(rules.size());
    std::iota(candidates.begin(), candidates.end(), std::uint32_t{0});
  } else {
    index.collectCandidates(element, candidates);
  }
}

/// The calling thread's candidate list, reused across elements to avoid an allocation per
/// cascade; nothing cascades another element while it is in use.
std::vector<std::uint32_t>& CandidateList() {
  thread_local std::vector<std::uint32_t> candidates;
  return candidates;
}

/// The candidate list for one element's cascade. On release it frees the list if the cascade grew
/// it past \ref StyleSystem::kRetainedCandidateCapacity, so the thread keeps at most that many
/// entries between cascades.
class CandidateListScope {
public:
  CandidateListScope() : list_(CandidateList()) {}
  ~CandidateListScope() {
    if (list_.capacity() > StyleSystem::kRetainedCandidateCapacity) {
      std::vector<std::uint32_t>().swap(list_);
    }
  }
  CandidateListScope(const CandidateListScope&) = delete;
  CandidateListScope& operator=(const CandidateListScope&) = delete;

  std::vector<std::uint32_t>& list() { return list_; }

private:
  std::vector<std::uint32_t>& list_;
};

struct MatchedSelectorEntry {
  std::size_t selectorEntryIndex = 0;
  css::Specificity specificity;
};

std::optional<MatchedSelectorEntry> MatchSelectorRule(const css::SelectorRule& rule,
                                                      const ShadowedElementAdapter& adapter,
                                                      css::SelectorTraversalBudget* budget,
                                                      const RuleIndex* index = nullptr,
                                                      std::uint32_t position = 0) {
  css::SelectorMatchOptions<ShadowedElementAdapter> options;
  options.traversalBudget = budget;
  for (std::size_t i = 0; i < rule.selector.entries.size(); ++i) {
    if (budget != nullptr && budget->rejected()) {
      return std::nullopt;
    }
    const auto& entry = rule.selector.entries[i];
    const bool matched = entry.matchesPattern(adapter, options);
    if (budget != nullptr && budget->rejected()) {
      return std::nullopt;
    }
    if (matched) {
      return MatchedSelectorEntry{
          .selectorEntryIndex = i,
          .specificity = css::Specificity(index ? index->specificity(position, i)
                                                : entry.computeSpecificity()),
      };
    }
  }

  return std::nullopt;
}

std::optional<MatchedSelectorEntry> MatchCascadeCandidate(const css::SelectorRule& rule,
                                                          const ShadowedElementAdapter& element,
                                                          css::SelectorTraversalBudget& traversal,
                                                          CascadeAncestorFilter* filter,
                                                          const RuleIndex& index,
                                                          std::uint32_t position, bool unindexed) {
  if (filter && filter->rejects(element.entity(), index, position)) {
    return std::nullopt;
  }
  return MatchSelectorRule(rule, element, &traversal, unindexed ? nullptr : &index, position);
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

template <typename Visitor>
void AdmitRuleDeclarations(const css::SelectorRule& rule, css::Specificity specificity,
                           StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                           Visitor&& visitor) {
  for (const auto& declaration : rule.declarations) {
    if (!styleBudget.reserveDeclarationApplication(declaration.values.size(),
                                                   declaration.sourceByteSize)) {
      return;
    }
    ++counters.declarationsApplied;
    visitor(declaration, specificity);
  }
}

void ParseRuleDeclaration(const css::Declaration& declaration, css::Specificity specificity,
                          PropertyRegistry& properties, ParseWarningSink& warningSink) {
  if (auto error = properties.parseProperty(declaration, specificity)) {
    constexpr std::size_t kMaximumStyleDiagnosticBytes = 4096;
    if (error->reason.size() > kMaximumStyleDiagnosticBytes) {
      error->reason =
          RcString(std::string_view(error->reason).substr(0, kMaximumStyleDiagnosticBytes));
    }
    warningSink.add(std::move(*error));
  }
}

const ComputedStyleComponent* FindRootStyle(const Registry& registry,
                                            const SVGDocumentContext* document) {
  if (document && registry.valid(document->rootEntity)) {
    return registry.try_get<ComputedStyleComponent>(document->rootEntity);
  }
  return nullptr;
}

const ComputedStyleComponent* FindParentStyle(const Registry& registry,
                                              const ExternalUseStyleContext* occurrence,
                                              Entity parent) {
  if (occurrence) {
    return &occurrence->parent;
  }
  return parent == entt::null ? nullptr : registry.try_get<ComputedStyleComponent>(parent);
}

bool RootStyleReady(const MatchedPropertiesKey& key, const SVGDocumentContext* document,
                    const Registry& registry, const ComputedStyleComponent* rootStyle) {
  return key.documentRoot || !document || !registry.valid(document->rootEntity) ||
         (rootStyle && rootStyle->style);
}

bool ParentStyleReady(const ExternalUseStyleContext* occurrence, Entity parent,
                      const ComputedStyleComponent* parentStyle) {
  return occurrence || parent == entt::null || (parentStyle && parentStyle->style);
}

void PopulateMatchedPropertiesKey(const Registry& registry,
                                  entt::basic_handle<const Registry> handle, Entity dataEntity,
                                  Entity parent, const StyleComponent* local,
                                  MatchedPropertiesKey& key) {
  const auto* type = registry.try_get<ElementTypeComponent>(dataEntity);
  key.localStyle = local;
  key.elementType = type ? std::optional(type->type()) : std::nullopt;
  key.noInheritPaint = parent != entt::null && registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
  key.instanceSymbol =
      type && type->type() == ElementType::Symbol && IsDirectUseInstanceRoot(handle);
  key.stop = StopOpacityInput(registry, dataEntity);
  key.flood = FloodOpacityInput(registry, dataEntity);
  const auto* document = registry.ctx().find<SVGDocumentContext>();
  key.documentRoot = document && document->rootEntity == handle.entity();
}

struct MatchedCacheState {
  const SVGDocumentContext* document = nullptr;
  const ComputedStyleComponent* rootStyle = nullptr;
  const ComputedStyleComponent* parentStyle = nullptr;
  bool parentReady = false;
  bool rootReady = false;
  bool cacheEnabled = false;
  bool initiallyCacheable = false;
  bool countedCacheMiss = false;
};

void SetMatchedKeyRecords(MatchedPropertiesKey& key, const MatchedCacheState& state) {
  key.parent = state.parentStyle ? state.parentStyle->style : nullptr;
  key.root = nullptr;
  if (!key.documentRoot && state.rootStyle) {
    key.root = state.rootStyle->style;
  }
}

/// External roots inherit the host paint; other elements respect the parent paint barrier.
bool CanInheritPaint(const Registry& registry, Entity parent,
                     const ExternalUseStyleContext* occurrence = nullptr) {
  return occurrence || parent == entt::null ||
         !registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
}

// Authored custom names sort before the supported element-specific property names.
bool HasLocalCustomProperties(const PropertyRegistry& properties) {
  const auto& deferred = properties.unparsedProperties;
  return !deferred.empty() && std::string_view(deferred.begin()->first).starts_with("--");
}

CustomPropertyMap::Action CustomPropertyAction(const css::Declaration& declaration) {
  using Action = CustomPropertyMap::Action;
  const auto params = parser::PropertyParseFnParams::Create(declaration, css::Specificity(),
                                                            parser::PropertyParseBehavior::Default);
  switch (params.explicitState) {
    case PropertyState::ExplicitInitial: return Action::Initial;
    case PropertyState::Inherit: return Action::Inherit;
    case PropertyState::ExplicitUnset: return Action::Unset;
    default: return Action::Set;
  }
}

using CustomTokens = std::span<const css::ComponentValue>;

std::optional<CustomTokens> InheritedVariable(const ComputedStyle& record, std::string_view name) {
  const auto* value = record.customProperties ? record.customProperties->find(name) : nullptr;
  return value ? std::optional(value->tokens()) : std::nullopt;
}

// Definitions borrow the winning declarations until the graph has published all its results.
// Each retained sequence is admitted immediately, before any subsequent expansion can reference it.
class CustomVariableOverlay {
public:
  void collect(const RcString& name, const css::Declaration& decl) {
    const auto action = CustomPropertyAction(decl);
    if (action == CustomPropertyMap::Action::Inherit ||
        action == CustomPropertyMap::Action::Unset) {
      return;
    }
    const bool initial = action == CustomPropertyMap::Action::Initial;
    definitions_.push_back({std::string_view(name),
                            initial ? std::nullopt : std::optional(CustomTokens(decl.values))});
    changes_.push_back({name, CustomPropertyMap::Action::Initial, nullptr});
    hasReferences_ |= !initial && decl.variableReferences != css::VariableReferences::None;
  }

  bool apply(ComputedStyle& record, StyleSystem::CascadePass& pass, ParseWarningSink& warnings) {
    const auto& account = pass.budget.retainedStyleBytes();
    if (hasReferences_) {
      if (!resolve(record, pass, warnings)) {
        return false;
      }
    } else {
      for (std::size_t i = 0; i < definitions_.size(); ++i) {
        if (definitions_[i].value && !store(i, *definitions_[i].value, account)) {
          return false;
        }
      }
    }
    auto next = CustomPropertyMap::Overlay(account, record.customProperties, changes_);
    if (!next) {
      return false;
    }
    record.customProperties = std::move(*next);
    return true;
  }

private:
  bool store(std::size_t i, CustomTokens value,
             const std::shared_ptr<RetainedStyleBytes>& account) {
    changes_[i].value = TokenSequence::Create(account, value);
    changes_[i].action = CustomPropertyMap::Action::Set;
    return bool(changes_[i].value);
  }

  bool resolve(const ComputedStyle& record, StyleSystem::CascadePass& pass,
               ParseWarningSink& warnings) {
    const css::VariableOutputs outputs{
        [&](std::size_t i) -> std::optional<CustomTokens> {
          return changes_[i].value ? std::optional(changes_[i].value->tokens()) : std::nullopt;
        },
        [&](std::size_t i, css::VariableSubstitutionResult value) {
          if (value.status == css::VariableSubstitutionStatus::LimitExceeded &&
              pass.budget.reportVariableLimitWarning()) {
            warnings.add(ParseDiagnostic::Error("CSS variable substitution limit exceeded",
                                                FileOffset::Offset(0)));
          }
          return value.status != css::VariableSubstitutionStatus::Resolved ||
                 store(i, value.values, pass.budget.retainedStyleBytes());
        }};
    return css::ResolveVariableDefinitions(
        definitions_, [&](std::string_view name) { return InheritedVariable(record, name); },
        outputs, {},
        [&](std::size_t components, std::size_t bytes) {
          return pass.budget.reserveGeneratedDeclarationWork(components, bytes);
        });
  }

  std::vector<css::VariableDefinition> definitions_;
  std::vector<CustomPropertyMap::Change> changes_;
  bool hasReferences_ = false;
};

[[gnu::noinline]] void ResolveCustomProperties(ComputedStyle& record,
                                               StyleSystem::CascadePass& pass,
                                               ParseWarningSink& warnings) {
  auto& deferred = record.properties.unparsedProperties;
  auto firstOrdinary = deferred.begin();
  CustomVariableOverlay overlay;
  while (firstOrdinary != deferred.end() &&
         std::string_view(firstOrdinary->first).starts_with("--")) {
    overlay.collect(firstOrdinary->first, firstOrdinary->second.declaration);
    ++firstOrdinary;
  }
  const bool accepted = !pass.budget.rejected() && overlay.apply(record, pass, warnings);
  deferred.erase(deferred.begin(), firstOrdinary);
  if (!accepted) {
    // A refused override must not expose the inherited value it replaced.
    record.customProperties.reset();
    pass.cache.clear();
    pass.budget.noteRetainedCustomPropertyRefusal();
  }
}

void RetainComputedRecord(StyleSystem::CascadePass& pass, ComputedStyle& record,
                          ComputedStyleComponent& computedStyle,
                          const ComputedResourceOpacities& resourceOpacities) {
  if (pass.staging) {
    record.resourceOpacities = resourceOpacities;
    return;
  }
  const std::size_t retainedBytes = record.properties.complexPropertyBytes();
  if (!pass.budget.canRetainComputedStyle(retainedBytes)) {
    pass.cache.clear();
  }
  if (pass.budget.retainComputedStyle(record, retainedBytes)) {
    record.resourceOpacities = resourceOpacities;
  } else {
    // Once the aggregate retained-byte budget is exhausted, use bounded initial styles for the
    // remainder of the pass instead of retaining attacker-sized local or inherited vectors.
    computedStyle.externalFill = false;
    computedStyle.externalStroke = false;
    record.properties = PropertyRegistry();
    record.customProperties.reset();
    record.resourceOpacities = ComputedResourceOpacities();
  }
}

bool FindAndInstallMatchedStyle(const Registry& registry, MatchedPropertiesCache& cache,
                                Entity parent, MatchedPropertiesKey& key,
                                std::size_t matchedDeclarationParsesSkipped,
                                StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                                ComputedStyleComponent& computed, MatchedCacheState& state) {
  const auto* parentRecord = state.parentStyle ? state.parentStyle->style.get() : nullptr;
  const auto* rootRecord =
      !key.documentRoot && state.rootStyle ? state.rootStyle->style.get() : nullptr;
  const MatchedPropertiesEntry* entry = cache.find(key, parentRecord, rootRecord);
  if (!entry || !ChargeCachedResourceOpacityWork(entry->style->properties, styleBudget, counters)) {
    return false;
  }
  const bool allowPaint = CanInheritPaint(registry, parent);
  computed.externalFill =
      ExternalPaintOrigin(entry->fillState, false, state.parentStyle, false, allowPaint);
  computed.externalStroke =
      ExternalPaintOrigin(entry->strokeState, false, state.parentStyle, true, allowPaint);
  computed.style = entry->style;
  ++counters.matchedPropertiesCacheHits;
  counters.declarationParsesSkipped += matchedDeclarationParsesSkipped;
  return true;
}

MatchedCacheState PrepareMatchedCacheState(const Registry& registry, Entity dataEntity,
                                           Entity parent, const ExternalUseStyleContext* occurrence,
                                           bool declarationsStreamed,
                                           const MatchedPropertiesKey& key,
                                           const StyleResourceBudget& styleBudget) {
  MatchedCacheState state;
  state.document = registry.ctx().find<SVGDocumentContext>();
  state.rootStyle = FindRootStyle(registry, state.document);
  state.parentStyle = FindParentStyle(registry, occurrence, parent);
  state.rootReady = RootStyleReady(key, state.document, registry, state.rootStyle);
  state.parentReady = ParentStyleReady(occurrence, parent, state.parentStyle);
  state.cacheEnabled = !registry.ctx().contains<DisableMatchedPropertiesCacheForTesting>();
  const bool intrinsicallyCacheable = !declarationsStreamed && !occurrence &&
                                      !registry.all_of<AnimatedValuesComponent>(dataEntity) &&
                                      !styleBudget.rejected() && key.cacheable();
  state.initiallyCacheable = state.cacheEnabled && intrinsicallyCacheable;
  return state;
}

bool TryInstallMatchedStyle(const Registry& registry, MatchedPropertiesCache& cache, Entity parent,
                            MatchedPropertiesKey& key, std::size_t deferredDeclarations,
                            StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                            ComputedStyleComponent& computed, MatchedCacheState& state) {
  if (!state.initiallyCacheable || !state.parentReady || !state.rootReady) {
    if (state.initiallyCacheable) {
      ++counters.matchedPropertiesCacheMisses;
      state.countedCacheMiss = true;
    } else if (state.cacheEnabled) {
      ++counters.matchedPropertiesCacheUncacheable;
    }
    return false;
  }
  if (!FindAndInstallMatchedStyle(registry, cache, parent, key, deferredDeclarations, styleBudget,
                                  counters, computed, state)) {
    ++counters.matchedPropertiesCacheMisses;
    state.countedCacheMiss = true;
    return false;
  }
  return true;
}

bool TryInstallLateMatchedStyle(const Registry& registry, MatchedPropertiesCache& cache,
                                Entity parent, MatchedPropertiesKey& key, bool warningBeforeParent,
                                StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                                ComputedStyleComponent& computed, MatchedCacheState& state) {
  if (!state.initiallyCacheable || warningBeforeParent || styleBudget.rejected()) {
    return false;
  }
  state.rootStyle = FindRootStyle(registry, state.document);
  state.rootReady = RootStyleReady(key, state.document, registry, state.rootStyle);
  if (!state.rootReady || !FindAndInstallMatchedStyle(registry, cache, parent, key, 0, styleBudget,
                                                      counters, computed, state)) {
    return false;
  }
  if (state.countedCacheMiss) {
    --counters.matchedPropertiesCacheMisses;
    state.countedCacheMiss = false;
  }
  return true;
}

bool FinishMatchedCacheMiss(const Registry& registry, MatchedPropertiesCache& cache,
                            MatchedPropertiesKey& key, PropertyState fillState,
                            PropertyState strokeState, bool warningBeforeParent,
                            std::size_t warningAttemptsAfterParent, ParseWarningSink& warningSink,
                            StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                            ComputedStyleComponent& computed, MatchedCacheState& state) {
  const bool warningAfterParent = warningSink.warningAttempts() != warningAttemptsAfterParent;
  if (state.countedCacheMiss &&
      (styleBudget.rejected() || warningBeforeParent || warningAfterParent)) {
    --counters.matchedPropertiesCacheMisses;
    ++counters.matchedPropertiesCacheUncacheable;
    state.countedCacheMiss = false;
  }
  if (!state.initiallyCacheable || styleBudget.rejected() || warningBeforeParent ||
      warningAfterParent) {
    return false;
  }
  state.rootStyle = FindRootStyle(registry, state.document);
  SetMatchedKeyRecords(key, state);
  return cache.insert(key, MatchedPropertiesEntry{computed.style, fillState, strokeState});
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

namespace {

constexpr std::size_t kWorkerWarningBytes = 256 * 1024;
constexpr std::size_t kPhaseReasonBytes = 32 * 1024;
constexpr std::size_t kPhaseWarningCount = 64;

struct WarningRange {
  std::size_t first = 0, count = 0, attempts = 0;
};

struct WarningPage;
struct ElementDiagnostics {
  const WarningPage* page = nullptr;
  WarningRange before, after;
};

/// One fixed allocation. Text is copied directly from the parser's temporary diagnostic.
struct WarningPage {
  struct Entry {
    DiagnosticSeverity severity;
    SourceRange range{FileOffset::Offset(0), FileOffset::Offset(0)};
    std::uint32_t offset, length;
  };

  explicit WarningPage(std::shared_ptr<const StyleSystem::PreparationLease> reservation)
      : lease(std::move(reservation)) {}

  bool append(const ParseDiagnostic& diagnostic) {
    const auto length = diagnostic.reason.size();
    if (count == entries.size() || length > text.size() - used) {
      return false;
    }
    auto& entry = entries[count++];
    entry = {diagnostic.severity, diagnostic.range, static_cast<std::uint32_t>(used),
             static_cast<std::uint32_t>(length)};
    std::copy_n(diagnostic.reason.data(), length, text.data() + used);
    used += length;
    return true;
  }

  bool matches(const WarningRange& range, const ParseWarningSink& result) const {
    if (result.resourceLimitExceeded() || result.warningAttempts() != range.attempts ||
        result.warnings().size() != range.count) {
      return false;
    }
    for (std::size_t i = 0; i < range.count; ++i) {
      const auto& saved = entries[range.first + i];
      const auto& actual = result.warnings()[i];
      if (saved.severity != actual.severity || saved.range != actual.range ||
          std::string_view(text.data() + saved.offset, saved.length) !=
              std::string_view(actual.reason)) {
        return false;
      }
    }
    return true;
  }

  bool commit(ParseWarningSink& destination, ParseWarningSink& canonical,
              const WarningRange& range) const {
    if (!matches(range, canonical)) {
      destination.merge(std::move(canonical));
      return false;
    }
    destination.mergeRecorded(range.count, range.attempts, false, [&](std::size_t i) {
      const auto& entry = entries[range.first + i];
      return ParseDiagnostic{entry.severity,
                             RcString(std::string_view(text.data() + entry.offset, entry.length)),
                             entry.range};
    });
    return range.attempts != 0;
  }

  std::shared_ptr<const StyleSystem::PreparationLease> lease;
  std::array<char, 64 * 1024> text;
  std::array<Entry, 256> entries;
  std::array<ElementDiagnostics, 32> groups;
  std::size_t used = 0, count = 0, groupCount = 0;
};

static_assert(sizeof(WarningPage) <= kWorkerWarningBytes);

/// Flush the first phase before serial parent recursion and the second on every return path.
class OrderedElementWarnings {
public:
  OrderedElementWarnings(ParseWarningSink& sink, const ElementDiagnostics* staged,
                         ParseWarningSink* workerAfter, std::size_t* used)
      : sink_(sink), staged_(staged), workerAfter_(workerAfter), used_(used) {
    if (staged) {
      canonical_.emplace(sink.remainingAllowance());
    }
  }

  ParseWarningSink& before() { return canonical_ ? *canonical_ : sink_; }
  ParseWarningSink& after() {
    if (staged_) {
      canonical_.emplace(sink_.remainingAllowance());
      return *canonical_;
    }
    return workerAfter_ ? *workerAfter_ : sink_;
  }
  void beforeParent() {
    if (staged_ && !flushed_) {
      *used_ += staged_->page->commit(sink_, *canonical_, staged_->before);
      canonical_.reset();
      flushed_ = true;
    }
  }
  ~OrderedElementWarnings() {
    if (staged_) {
      beforeParent();
      if (canonical_) {
        *used_ += staged_->page->commit(sink_, *canonical_, staged_->after);
      }
    }
  }

private:
  ParseWarningSink& sink_;
  const ElementDiagnostics* staged_;
  ParseWarningSink* workerAfter_;
  std::size_t* used_;
  std::optional<ParseWarningSink> canonical_;
  bool flushed_ = false;
};

}  // namespace

/// A bounded match transcript borrows immutable rules until the pass owner rejoins.
struct StyleSystem::RuleTrace {
  struct Match {
    std::uint64_t revision;
    std::size_t ordinal;
    std::uint32_t position;
    const css::SelectorRule* rule;
    css::Specificity specificity;
  };

  // A prior full-key hit or successful insertion already set the clock bit. An insertion
  // can move bucket entries even without evicting one, so retain the installed slot instead.
  struct CacheHit {
    const ComputedStyleComponent* installed = nullptr;
    const ComputedStyle* parent = nullptr;
    const ComputedStyle* root = nullptr;
    std::uint64_t epoch = 0;
  };

  CacheHit* hit = nullptr;
  const ComputedStyleComponent* parentSlot = nullptr;
  Entity parentEntity = entt::null;
  const ComputedStyleComponent* rootSlot = nullptr;
  bool needsRoot = false;
  bool needsParent = false;

  bool canonicalContextMatches(const ComputedStyleComponent* parent, const CacheHit& saved) const {
    const auto* parentRecord = parent ? parent->style.get() : nullptr;
    const auto* rootRecord = rootSlot ? rootSlot->style.get() : nullptr;
    return (!needsParent || parentRecord) && (!needsRoot || rootRecord) &&
           parentRecord == saved.parent && rootRecord == saved.root &&
           (!parent || (!parent->externalFill && !parent->externalStroke));
  }

  bool canUsePriorHit(const StyleSystem::CascadePass& pass,
                      const ComputedStyleComponent* parent) const {
    return hit && hit->installed && pass.cache.evictionEpoch() == hit->epoch &&
           canonicalContextMatches(parent, *hit);
  }

  static bool mayDeferMaterialization(const RuleTrace* trace, const MatchedCacheState& state) {
    return trace && trace->replayed && !trace->diagnostics && !state.parentReady &&
           state.initiallyCacheable;
  }

  void replayAncestorWork(StyleAncestorFilter* filter, Entity entity,
                          StyleCascadeCounters& counters) const {
    if (filterSynchronized) {
      UTILS_RELEASE_ASSERT(filter);
      filter->synchronizeTo(entity);
      counters.ancestorFilterRejections += filterRejections;
      counters.ancestorFilterFalsePositives += filterFalsePositives;
    }
  }

  bool usePriorHit(StyleSystem::CascadePass& pass, Entity entity) {
    if (!canUsePriorHit(pass, parentSlot) ||
        !pass.budget.tryReplayCacheHit(work.rules, work.applications, work.components, work.bytes,
                                       work.traversal)) {
      return false;
    }
    replayAncestorWork(pass.ancestorFilter, entity, pass.counters);
    installPriorHit(pass.counters);
    return true;
  }

  void installPriorHit(StyleCascadeCounters& counters) {
    *slot = *hit->installed;
    priorHitReused = true;
    ++counters.elementsCascaded;
    counters.rulesTested += work.rules;
    counters.rulesMatched += count;
    counters.declarationsApplied += work.applications;
    ++counters.matchedPropertiesCacheHits;
    counters.declarationParsesSkipped += work.applications;
    replayed = true;
  }

  static void rememberHit(RuleTrace* trace, const MatchedPropertiesCache& cache) {
    if (!trace || !trace->hit || trace->slot->externalFill || trace->slot->externalStroke ||
        !trace->slot->properties()->unparsedProperties.empty()) {
      return;
    }
    *trace->hit = {trace->slot, trace->parentSlot ? trace->parentSlot->style.get() : nullptr,
                   trace->rootSlot ? trace->rootSlot->style.get() : nullptr, cache.evictionEpoch()};
  }

  static void rememberCacheOutcome(RuleTrace* capture, RuleTrace* replay,
                                   const MatchedPropertiesCache& cache) {
    rememberHit(replay, cache);
    if (capture) {
      capture->cacheableStyle = true;
    }
  }

  static OrderedElementWarnings diagnosticsFor(RuleTrace* replay, ParseWarningSink& sink,
                                               ParseWarningSink* after) {
    return OrderedElementWarnings(sink, replay ? replay->diagnostics.get() : nullptr, after,
                                  replay ? &replay->reusedWarningPhases : nullptr);
  }

  static void bindPreparedInputs(RuleTrace* capture, const Registry& registry, Entity parent,
                                 const MatchedPropertiesKey& key, const MatchedCacheState& state) {
    if (capture) {
      capture->parentSlot = state.parentStyle;
      capture->parentEntity = parent;
      capture->needsParent = parent != entt::null;
      capture->needsRoot =
          !key.documentRoot && state.document && registry.valid(state.document->rootEntity);
      capture->rootSlot = capture->needsRoot ? state.rootStyle : nullptr;
    }
    UTILS_RELEASE_ASSERT_MSG(state.parentReady, "prepared style parent must be published");
    UTILS_RELEASE_ASSERT_MSG(state.rootReady || (capture && capture->rootPrefix),
                             "prepared style root must be published");
  }

  struct Work {
    std::size_t rules, applications, components, bytes, traversal;

    static Work read(const StyleResourceBudget& budget) {
      return {budget.ruleElementMatches(), budget.declarationApplications(),
              budget.declarationComponentWork(), budget.declarationByteWork(),
              budget.selectorTraversal().steps()};
    }
    Work operator-(const Work& before) const {
      return {rules - before.rules, applications - before.applications,
              components - before.components, bytes - before.bytes, traversal - before.traversal};
    }

    bool tryChargeTogether(const Work& other, StyleResourceBudget& budget) const {
      constexpr auto maximum = std::numeric_limits<std::size_t>::max();
      if (other.rules > maximum - rules || other.applications > maximum - applications ||
          other.components > maximum - components || other.bytes > maximum - bytes ||
          other.traversal > maximum - traversal) {
        return false;
      }
      return budget.tryReplayCacheHit(rules + other.rules, applications + other.applications,
                                      components + other.components, bytes + other.bytes,
                                      traversal + other.traversal);
    }
  };

  struct Capture {
    RuleTrace* trace;
    const StyleResourceBudget& budget;
    Work before;
    const AncestorFilterCheck& filter;
    const StyleCascadeCounters& counters;
    std::uint64_t rejections;
    std::uint64_t falsePositives;
    Capture(RuleTrace* trace, const StyleResourceBudget& budget, const AncestorFilterCheck& filter,
            const StyleCascadeCounters& counters)
        : trace(trace),
          budget(budget),
          before(trace ? Work::read(budget) : Work{}),
          filter(filter),
          counters(counters),
          rejections(counters.ancestorFilterRejections),
          falsePositives(counters.ancestorFilterFalsePositives) {}
    void append(Match match) const {
      if (trace) {
        trace->append(match);
      }
    }
    ~Capture() {
      if (trace) {
        trace->work = Work::read(budget) - before;
        trace->filterSynchronized = filter.synchronized();
        trace->filterRejections = counters.ancestorFilterRejections - rejections;
        trace->filterFalsePositives = counters.ancestorFilterFalsePositives - falsePositives;
        trace->complete = !budget.rejected() && trace->count <= trace->matches.size();
      }
    }
  };

  template <typename Accept>
  bool reuseTagMatches(StyleResourceBudget& budget, StyleCascadeCounters& counters,
                       Accept&& accept) {
    const auto* witness = tagWitness;
    if (!witness ||
        !budget.canReserveDeclarationWork(witness->work.applications, witness->work.components,
                                          witness->work.bytes) ||
        !budget.tryReplaySelectorWork(witness->work.rules, witness->work.traversal)) {
      return false;
    }
    work = witness->work;
    borrowedMatches = witness->recordedMatches();
    count = witness->count;
    complete = true;
    counters.rulesTested += work.rules;
    for (std::size_t i = 0; i < count; ++i) {
      accept(recordedMatches()[i]);
    }
    return true;
  }

  template <typename Accept>
  static bool reuseSelectorWork(RuleTrace* capture, RuleTrace* replay, Entity treeEntity,
                                StyleResourceBudget& budget, StyleCascadeCounters& counters,
                                StyleAncestorFilter* filter, const Accept& accept) {
    if (replay && replay->replayMatches(budget, counters, accept)) {
      replay->replayAncestorWork(filter, treeEntity, counters);
      return true;
    }
    return capture && capture->reuseTagMatches(budget, counters, accept);
  }

  static std::pair<bool*, bool*> observation(RuleTrace* capture) {
    if (!capture) {
      return {};
    }
    return {capture->subjectOnly ? &capture->subjectOnly : nullptr,
            capture->tagOnly ? &capture->tagOnly : nullptr};
  }

  void append(Match match) {
    if (count < matches.size()) {
      matches[count++] = match;
    } else {
      count = matches.size() + 1;
    }
  }

  bool tryCharge(StyleResourceBudget& budget) {
    replayed = complete &&
               budget.canReserveDeclarationWork(work.applications, work.components, work.bytes) &&
               budget.tryReplaySelectorWork(work.rules, work.traversal);
    return replayed;
  }

  template <typename Accept>
  bool replayMatches(StyleResourceBudget& budget, StyleCascadeCounters& counters, Accept&& accept) {
    if (!tryCharge(budget)) {
      return false;
    }
    counters.rulesTested += work.rules;
    for (std::size_t i = 0; i < count; ++i) {
      accept(recordedMatches()[i]);
    }
    return true;
  }

  std::array<Match, 4> matches{};
  // Both traces stay in this batch until owner replay finishes.
  const Match* borrowedMatches = nullptr;

  const Match* recordedMatches() const {
    return borrowedMatches ? borrowedMatches : matches.data();
  }
  std::size_t count = 0;
  Work work{};
  ComputedStyleComponent* slot = nullptr;
  bool originalExternalFill = false;
  bool originalExternalStroke = false;

  void restoreOriginalSlot() const {
    slot->style.reset();
    slot->externalFill = originalExternalFill;
    slot->externalStroke = originalExternalStroke;
  }
  bool complete = false;
  bool replayed = false;
  bool priorHitReused = false;
  bool rootPrefix = false;
  static constexpr std::uint32_t kNoBorrowedStyle = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t borrowedStyle = kNoBorrowedStyle;
  bool publishSlot = true;
  bool subjectOnly = false;
  bool cacheableStyle = false;
  bool tagOnly = false;
  const RuleTrace* tagWitness = nullptr;
  const xml::XMLQualifiedName* tag = nullptr;
  ElementType tagElementType = ElementType::Unknown;
  bool svgNamespace = false;
  bool defaultTagStyle = false;
  bool filterSynchronized = false;
  std::uint64_t filterRejections = 0;
  std::uint64_t filterFalsePositives = 0;
  std::shared_ptr<const ElementDiagnostics> diagnostics;
  std::size_t reusedWarningPhases = 0;
};

/// Each entity's work has one dense slot. Both arrays are frozen while workers are running.
struct StyleSystem::RuleTraces {
  entt::sparse_set ownedIndex;
  const entt::sparse_set* preparedIndex = nullptr;
  const entt::sparse_set& entities() const { return preparedIndex ? *preparedIndex : ownedIndex; }
  std::vector<RuleTrace> entries;

  bool collectRootChain(const Registry& view, Entity entity, std::vector<Entity>& chain) {
    while (entity != entt::null) {
      const auto* slot = view.try_get<ComputedStyleComponent>(entity);
      if (slot && slot->style) {
        break;
      }
      auto* trace = find(entity);
      if (!trace || trace->rootPrefix) {
        return false;
      }
      trace->rootPrefix = true;
      chain.push_back(entity);
      entity = view.get<donner::components::TreeComponent>(entity).parent();
    }
    return true;
  }

  void clear() {
    entries.clear();
    ownedIndex.clear();
  }

  RuleTrace* find(Entity entity) {
    const auto& index = entities();
    return index.contains(entity) ? &entries[index.index(entity)] : nullptr;
  }
};

namespace {
std::unique_ptr<CascadeAncestorFilter> MakeAncestorFilter(const Registry& registry,
                                                          StyleCascadeCounters& counters) {
  const auto* provider = registry.ctx().find<CascadeAncestorFilters>();
  return provider && provider->create ? provider->create(registry, counters) : nullptr;
}
}  // namespace

/// Owns the serial-only operations for one synchronous cascade, including missing parents.
class StyleSystem::SerialCascade {
public:
  SerialCascade(Registry& registry, ParseWarningSink& warnings, RuleTraces* traces = nullptr,
                bool enableFilter = false, bool enableTestFilter = false,
                const std::unique_ptr<DocumentLayerOrder>* preparedLayers = nullptr)
      : registry_(registry),
        warnings_(warnings),
        traces_(traces),
        layers_(preparedLayers ? nullptr : DocumentLayerOrder::Build(registry)),
        pass_{GetStyleResourceBudget(registry), GetStyleCascadeMetrics(registry).counters,
              GetMatchedPropertiesCache(registry)},
        ancestorFilter_(registry, pass_.counters, enableFilter) {
    pass_.layers = preparedLayers ? preparedLayers->get() : layers_.get();
    if (pass_.layers) {
      pass_.layers->reportRefusal(warnings);
    }
    pass_.ancestorFilter = ancestorFilter_.get();
    if (enableTestFilter) {
      testFilter_ = MakeAncestorFilter(registry, pass_.counters);
      pass_.testFilter = testFilter_.get();
    }
  }

  const ComputedStyleComponent& compute(Entity entity) {
    auto* trace = traces_ ? traces_->find(entity) : nullptr;
    auto& slot = trace ? *trace->slot : registry_.get_or_emplace<ComputedStyleComponent>(entity);
    if (slot.style) {
      return slot;
    }
    if (!trace || !(trace->usePriorHit(pass_, entity) || usePriorHitsWithParent(*trace, entity))) {
      computePropertiesInto(registry_, entity, slot, pass_, warnings_, this, nullptr, nullptr,
                            trace);
    }
    if (trace) {
      countTrace(*trace);
    }
    return slot;
  }

  std::size_t countRecordedWork(StyleCascadeMetrics& metrics) const {
    metrics.preparedWarningPhases += reusedWarningPhases_;
    metrics.reusedCanonicalHits += reusedCanonicalHits_;
    return acceptedTraces_;
  }

  static void publishMissingParent(SerialCascade* serial, Entity parent, MatchedCacheState& state) {
    if (!state.parentReady) {
      UTILS_RELEASE_ASSERT(serial);
      state.parentStyle = &serial->compute(parent);
    }
  }

  AnimationResourceBudget* animationBudget() {
    return registry_.ctx().find<AnimationResourceBudget>();
  }

  void rejectExternalResource() { StyleSystem::rejectExternalHostResource(registry_); }

private:
  void countTrace(const RuleTrace& trace) {
    acceptedTraces_ += trace.replayed;
    reusedWarningPhases_ += trace.reusedWarningPhases;
    reusedCanonicalHits_ += trace.priorHitReused;
  }

  bool usePriorHitsWithParent(RuleTrace& child, Entity entity) {
    if (!child.hit || !child.hit->installed || !child.parentSlot || child.parentSlot->style) {
      return false;
    }
    auto* parent = traces_->find(child.parentEntity);
    if (!parent || !parent->canUsePriorHit(pass_, parent->parentSlot) ||
        !child.canUsePriorHit(pass_, parent->hit->installed) ||
        !child.work.tryChargeTogether(parent->work, pass_.budget)) {
      return false;
    }
    // The child has already paid the work that precedes parent recursion. Both probes would
    // hit marked entries and leave the clock untouched, so install the parent before the child.
    child.replayAncestorWork(pass_.ancestorFilter, entity, pass_.counters);
    parent->replayAncestorWork(pass_.ancestorFilter, child.parentEntity, pass_.counters);
    parent->installPriorHit(pass_.counters);
    countTrace(*parent);
    child.installPriorHit(pass_.counters);
    return true;
  }

  Registry& registry_;
  ParseWarningSink& warnings_;
  RuleTraces* traces_;
  std::unique_ptr<DocumentLayerOrder> layers_;
  CascadePass pass_;
  PassAncestorFilter ancestorFilter_;
  std::unique_ptr<CascadeAncestorFilter> testFilter_;
  std::size_t acceptedTraces_ = 0;
  std::size_t reusedWarningPhases_ = 0;
  std::size_t reusedCanonicalHits_ = 0;
};

class StyleSystem::PreparationLease {
public:
  PreparationLease(std::shared_ptr<RetainedStyleBytes> account, std::size_t bytes)
      : account_(std::move(account)), bytes_(bytes) {}
  ~PreparationLease() { account_->releasePreparation(bytes_); }
  PreparationLease(const PreparationLease&) = delete;
  PreparationLease& operator=(const PreparationLease&) = delete;

private:
  std::shared_ptr<RetainedStyleBytes> account_;
  std::size_t bytes_;
};

namespace {
struct ShadowStyleWitness {
  Entity data = entt::null;
  const xml::XMLQualifiedName* tag = nullptr;
  const ComputedStyle* parent = nullptr;
  bool noInheritPaint = false;
};
struct ShadowParentWitness {
  Entity entity = entt::null;
  const ComputedStyleComponent* style = nullptr;
  bool noInheritPaint = false;
};

constexpr std::size_t kShadowStyleWitnesses = 64;
}  // namespace

std::size_t StyleSystem::PreparedBatch::workerReservationBytes() {
  return StagedRecords::kMaximumBytes + MatchedPropertiesCache::workerMetadataBytes() +
         sizeof(WarningPage) + sizeof(ShadowParentWitness) +
         kShadowStyleWitnesses * (sizeof(ShadowStyleWitness) + 2 * sizeof(void*));
}

class StyleSystem::PreparedBatch::Worker {
public:
  friend class PreparedBatch;
  Worker(const PreparedBatch* owner, const Registry& registry, const StyleResourceBudget& logical,
         StyleResourceBudget::Limits share, std::shared_ptr<const PreparationLease> lease,
         bool enableFilter, std::uint32_t firstRecord, const DocumentLayerOrder* layers)
      : lease_(std::move(lease)),
        owner_(owner),
        budget_(logical, share),
        cache_(MatchedPropertiesCache::Scope::Worker),
        records_(lease_, firstRecord),
        pass_{budget_, counters_, cache_, 0, &records_},
        ancestorFilter_(registry, counters_, enableFilter),
        testFilter_(MakeAncestorFilter(registry, counters_)),
        recordMemoEnabled_(!testFilter_ &&
                           !registry.ctx().contains<DisableMatchedPropertiesCacheForTesting>()) {
    pass_.ancestorFilter = ancestorFilter_.get();
    pass_.testFilter = testFilter_.get();
    pass_.layers = layers;
  }

private:
  std::shared_ptr<const PreparationLease> lease_;
  const PreparedBatch* owner_;
  StyleResourceBudget budget_;
  StyleCascadeCounters counters_;
  MatchedPropertiesCache cache_;
  StagedRecords records_;
  CascadePass pass_;
  PassAncestorFilter ancestorFilter_;
  std::unique_ptr<CascadeAncestorFilter> testFilter_;
  std::shared_ptr<WarningPage> warnings_;
  const bool recordMemoEnabled_;

  struct ShadowMemo {
    ShadowStyleWitness key;
    const RuleTrace* trace = nullptr;
  };
  std::array<ShadowMemo, kShadowStyleWitnesses> shadowStyles_;

  std::array<const RuleTrace*, kShadowStyleWitnesses> tagWitnesses_{};

  static std::size_t tagBucket(const xml::XMLQualifiedName& tag) {
    return std::hash<std::string_view>{}(std::string_view(tag.name)) % kShadowStyleWitnesses;
  }

  static bool defaultStyleInputs(const Registry& registry, Entity entity,
                                 const StyleComponent* local) {
    return (!local || local->hasEmptyDeclarations()) &&
           !registry.any_of<StopComponent, FEFloodComponent, FEDropShadowComponent>(entity);
  }

  static bool sameParentStyle(const ComputedStyleComponent* parent, const RuleTrace& witness) {
    const auto* previous = witness.parentSlot;
    return parent && parent->style && previous && parent->style.get() == previous->style.get() &&
           parent->externalFill == previous->externalFill &&
           parent->externalStroke == previous->externalStroke;
  }

  bool reuseTagStyle(const Registry& registry, Entity entity, const StyleComponent* local,
                     RuleTrace& trace) {
    const auto* witness = trace.tagWitness;
    if (!recordMemoEnabled_ || !witness || !witness->defaultTagStyle || !witness->needsRoot ||
        !defaultStyleInputs(registry, entity, local)) {
      return false;
    }
    const Entity parentEntity = registry.get<donner::components::TreeComponent>(entity).parent();
    const auto* parent = registry.try_get<ComputedStyleComponent>(parentEntity);
    if (!sameParentStyle(parent, *witness) ||
        registry.all_of<DoNotInheritFillOrStrokeTag>(parentEntity) !=
            registry.all_of<DoNotInheritFillOrStrokeTag>(witness->parentEntity)) {
      return false;
    }
    const auto* document = registry.ctx().find<SVGDocumentContext>();
    return document && document->rootEntity != entity &&
           installWitness(*witness, parentEntity, *parent, trace);
  }

  void setupTagWitness(const Registry& registry, Entity entity, RuleTrace& trace) {
    if (testFilter_) {
      return;
    }
    const auto* type = registry.try_get<ElementTypeComponent>(entity);
    if (!type) {
      return;
    }
    trace.tag = &registry.get<donner::components::TreeComponent>(entity).tagName();
    trace.tagElementType = type->type();
    trace.svgNamespace = type->inSvgNamespace();
    trace.tagOnly = true;
    const auto* candidate = tagWitnesses_[tagBucket(*trace.tag)];
    if (candidate && candidate->tagElementType == trace.tagElementType &&
        candidate->svgNamespace == trace.svgNamespace && *candidate->tag == *trace.tag) {
      trace.tagWitness = candidate;
    }
  }

  void rememberTagWitness(const Registry& registry, Entity entity, const StyleComponent* local,
                          RuleTrace& trace) {
    if (trace.tagOnly && trace.complete && !trace.filterSynchronized && !trace.tagWitness) {
      trace.defaultTagStyle = recordMemoEnabled_ && trace.cacheableStyle && !trace.diagnostics &&
                              trace.slot->style &&
                              trace.slot->properties()->unparsedProperties.empty() &&
                              defaultStyleInputs(registry, entity, local);
      tagWitnesses_[tagBucket(*trace.tag)] = &trace;
    }
  }

  bool installShadowStyle(const ShadowStyleWitness& key, Entity parentEntity,
                          const ComputedStyleComponent& parent, RuleTrace& trace) {
    auto& entry = shadowStyles_[static_cast<std::uint32_t>(key.data) % shadowStyles_.size()];
    const auto* previous = entry.trace;
    if (!previous || entry.key.data != key.data || *entry.key.tag != *key.tag ||
        entry.key.parent != key.parent || entry.key.noInheritPaint != key.noInheritPaint) {
      return false;
    }
    return installWitness(*previous, parentEntity, parent, trace);
  }

  bool installWitness(const RuleTrace& previous, Entity parentEntity,
                      const ComputedStyleComponent& parent, RuleTrace& trace) {
    const auto& work = previous.work;
    if (!budget_.tryReplayCacheHit(work.rules, work.applications, work.components, work.bytes,
                                   work.traversal)) {
      return false;
    }
    trace.parentSlot = &parent;
    trace.parentEntity = parentEntity;
    trace.rootSlot = previous.rootSlot;
    trace.needsRoot = previous.needsRoot;
    trace.needsParent = true;
    trace.work = work;
    trace.borrowedMatches = previous.recordedMatches();
    trace.count = previous.count;
    trace.complete = true;
    if (trace.publishSlot) {
      *trace.slot = *previous.slot;
    } else {
      trace.borrowedStyle = previous.slot->style->cachePlacement;
    }
    ++counters_.elementsCascaded;
    counters_.rulesTested += work.rules;
    counters_.rulesMatched += trace.count;
    counters_.declarationsApplied += work.applications;
    ++counters_.matchedPropertiesCacheHits;
    counters_.declarationParsesSkipped += work.applications;
    return true;
  }

  ShadowParentWitness lastParent_;

  const ShadowParentWitness* readyShadowParent(const Registry& registry, Entity entity) {
    if (lastParent_.style && lastParent_.entity == entity) {
      return &lastParent_;
    }
    const auto* style = registry.try_get<ComputedStyleComponent>(entity);
    if (!style || !style->style || style->externalFill || style->externalStroke) {
      return nullptr;
    }
    lastParent_ = {entity, style, registry.all_of<DoNotInheritFillOrStrokeTag>(entity)};
    return &lastParent_;
  }

  bool reuseShadowStyle(const Registry& registry, Entity entity, Entity data, RuleTrace& trace) {
    if (!recordMemoEnabled_ || registry.all_of<ShadowTreeRootComponent>(entity)) {
      return false;
    }
    const auto& tree = registry.get<donner::components::TreeComponent>(entity);
    const auto* parent = readyShadowParent(registry, tree.parent());
    if (!parent) {
      return false;
    }
    trace.subjectOnly = true;
    return installShadowStyle(
        {data, &tree.tagName(), parent->style->style.get(), parent->noInheritPaint}, parent->entity,
        *parent->style, trace);
  }

  void rememberShadowStyle(const Registry& registry, Entity entity, Entity data,
                           const RuleTrace& trace) {
    if (!trace.subjectOnly || !trace.cacheableStyle || !trace.complete || trace.diagnostics ||
        trace.filterSynchronized || !trace.slot->style ||
        !trace.slot->properties()->unparsedProperties.empty()) {
      return;
    }
    const auto& tree = registry.get<donner::components::TreeComponent>(entity);
    auto& entry = shadowStyles_[static_cast<std::uint32_t>(data) % shadowStyles_.size()];
    entry = {{data, &tree.tagName(), trace.parentSlot->style.get(),
              registry.all_of<DoNotInheritFillOrStrokeTag>(tree.parent())},
             &trace};
  }

  bool hasAdmittedShadowInput(Entity data) const {
    const auto& memo = shadowStyles_[static_cast<std::uint32_t>(data) % shadowStyles_.size()];
    // This entry was recorded after local preflight. The light entity and its declaration
    // component are immutable for this batch, so its local proof applies to another clone.
    return memo.trace && memo.key.data == data;
  }

  const StyleComponent* localStyleForPreflight(const Registry& registry, Entity entity,
                                               const ShadowEntityComponent* shadow) const {
    if (!shadow) {
      return registry.try_get<StyleComponent>(entity);
    }
    const Entity data = shadow->lightEntity;
    // If reuse fails, ordinary style computation resolves its own local component.
    return hasAdmittedShadowInput(data) ? nullptr : registry.try_get<StyleComponent>(data);
  }

  bool tryReuseStyle(const Registry& registry, Entity entity, const ShadowEntityComponent* shadow,
                     const StyleComponent* local, RuleTrace& trace) {
    // A root-prefix record may have been computed without the document root's font context.
    if (trace.rootPrefix) {
      return false;
    }
    if (shadow) {
      return reuseShadowStyle(registry, entity, shadow->lightEntity, trace);
    }
    setupTagWitness(registry, entity, trace);
    return reuseTagStyle(registry, entity, local, trace);
  }

  void rememberStyle(const Registry& registry, Entity entity, const ShadowEntityComponent* shadow,
                     const StyleComponent* local, RuleTrace& trace) {
    if (trace.rootPrefix) {
      return;
    }
    if (shadow) {
      rememberShadowStyle(registry, entity, shadow->lightEntity, trace);
    } else {
      rememberTagWitness(registry, entity, local, trace);
    }
  }

  WarningPage& warningPage() {
    if (!warnings_) {
      // A retained weak observer may outlive the page and its reservation.
      warnings_ = std::shared_ptr<WarningPage>(new WarningPage(lease_));
    }
    return *warnings_;
  }

  ParseWarningSink sink(bool enabled) {
    if (!enabled) {
      return ParseWarningSink::Disabled();
    }
    return ParseWarningSink::Streaming(
        kPhaseWarningCount, kPhaseReasonBytes, this,
        [](void* worker, const ParseDiagnostic& value) {
          return static_cast<Worker*>(worker)->warningPage().append(value);
        });
  }

  bool keepWarnings(RuleTrace& trace, std::size_t first, const ParseWarningSink& early,
                    const ParseWarningSink& late) {
    if (early.resourceLimitExceeded() || late.resourceLimitExceeded()) {
      return false;
    }
    if (!early.warningAttempts() && !late.warningAttempts()) {
      return true;
    }
    auto& page = warningPage();
    if (page.groupCount == page.groups.size()) {
      return false;
    }
    auto& saved = page.groups[page.groupCount++];
    saved = {&page,
             {first, early.retainedCount(), early.warningAttempts()},
             {first + early.retainedCount(), late.retainedCount(), late.warningAttempts()}};
    trace.diagnostics = std::shared_ptr<const ElementDiagnostics>(warnings_, &saved);
    return true;
  }
};

struct StyleSystem::PreparedBatch::Impl {
  Impl(Registry& registry, std::size_t contexts)
      : registry(registry),
        logical(GetStyleResourceBudget(registry)),
        layers(DocumentLayerOrder::Build(registry)),
        contexts(contexts),
        share(logical.remainingWorkLimits(logical.limits())) {
    UTILS_RELEASE_ASSERT(contexts > 0);
    share.ruleElementMatches /= contexts;
    share.declarationApplications /= contexts;
    share.declarationComponentWork /= contexts;
    share.declarationByteWork /= contexts;
    share.selectorTraversalSteps /= contexts;
  }

  Registry& registry;
  const StyleResourceBudget& logical;
  const std::unique_ptr<DocumentLayerOrder> layers;
  const std::size_t contexts;
  StyleResourceBudget::Limits share;
  std::mutex workerMutex;
  std::unordered_map<std::thread::id, std::unique_ptr<Worker>> workers;
  std::atomic<bool> abandoned = false;
  RuleTraces traces;
  std::size_t reservationBytes() const {
    const auto* filters = std::as_const(registry).ctx().find<CascadeAncestorFilters>();
    const auto base =
        workerReservationBytes() +
        (enableFilter && !filters && !registry.ctx().contains<UnfilteredCascadeForTesting>()
             ? StyleAncestorFilter::maximumRetainedBytes()
             : 0);
    if (!filters || !filters->create) {
      return base;
    }
    if (filters->maximumBytesPerWorker > std::numeric_limits<std::size_t>::max() - base) {
      return std::numeric_limits<std::size_t>::max();
    }
    return base + filters->maximumBytesPerWorker;
  }

  bool collectWarnings = true;
  bool enableFilter = false;
  bool finished = false;

  void restoreSlots(std::span<RuleTrace::CacheHit> hits = {}) {
    for (auto& trace : traces.entries) {
      const auto& style = trace.slot->style;
      const auto index = style ? style->cachePlacement : trace.borrowedStyle;
      if (!hits.empty() && trace.complete && !trace.diagnostics &&
          index != RuleTrace::kNoBorrowedStyle) {
        UTILS_RELEASE_ASSERT(index < hits.size());
        trace.hit = &hits[index];
      }
      if (style) {
        trace.restoreOriginalSlot();
      }
    }
  }
};

namespace {
template <typename... Types>
void WarmCascadeTypes() {
  static const bool ready = [] {
    (static_cast<void>(entt::type_id<Types>()), ...);
    return true;
  }();
  static_cast<void>(ready);
}
}  // namespace

StyleSystem::PreparedBatch::PreparedBatch(Registry& registry, std::span<const Entity> entities,
                                          std::size_t contexts, bool collectWarnings,
                                          bool enableFilter, const entt::sparse_set* preparedIndex)
    : impl_(std::make_unique<Impl>(registry, contexts)) {
  impl_->collectWarnings = collectWarnings;
  impl_->enableFilter = enableFilter;
  UTILS_RELEASE_ASSERT(entities.size() <= kMaximumElements);
  UTILS_RELEASE_ASSERT(!registry.ctx().contains<ExternalUseStyleContext>());
  UTILS_RELEASE_ASSERT(std::as_const(registry).view<const AnimatedValuesComponent>().empty());
  // Type IDs can be lazily numbered even when a lookup finds no context entry.
  WarmCascadeTypes<
      donner::components::TreeComponent, donner::components::AttributesComponent,
      ElementTypeComponent, IdComponent, ClassComponent, StyleComponent, StylesheetComponent,
      ShadowEntityComponent, ShadowTreeRootComponent, xml::components::XMLNodeTypeComponent,
      xml::components::XMLValueComponent, StopComponent, FEFloodComponent, FEDropShadowComponent,
      FEDiffuseLightingComponent, FESpecularLightingComponent, DoNotInheritFillOrStrokeTag,
      DescriptiveTextComponent, TextComponent, ProjectedNonWhitespaceTextTag,
      SelectorRawTextAuthorityTag, SelectorRejectedTextTag, ComputedStyleComponent,
      AnimatedValuesComponent, SVGDocumentContext, AnimationStyleContext,
      DisableMatchedPropertiesCacheForTesting, UnindexedCascadeForTesting, ExternalUseStyleContext,
      StyleResourceBudget, StyleCascadeMetrics, MatchedPropertiesCache, CascadeAncestorFilters,
      UnfilteredCascadeForTesting>();
  auto& traces = impl_->traces;
  traces.preparedIndex = preparedIndex;
  if (preparedIndex) {
    UTILS_RELEASE_ASSERT(preparedIndex->data() == entities.data() &&
                         preparedIndex->size() == entities.size());
  } else {
    traces.ownedIndex.reserve(entities.size());
  }
  traces.entries.reserve(entities.size());
  for (Entity entity : entities) {
    auto& slot = registry.get_or_emplace<ComputedStyleComponent>(entity);
    UTILS_RELEASE_ASSERT(!slot.style);
    if (!preparedIndex) {
      UTILS_RELEASE_ASSERT(!traces.ownedIndex.contains(entity));
      std::ignore = traces.ownedIndex.push(entity);
    }
    auto& trace = traces.entries.emplace_back();
    trace.slot = &slot;
    trace.originalExternalFill = slot.externalFill;
    trace.originalExternalStroke = slot.externalStroke;
  }
}

StyleSystem::PreparedBatch::~PreparedBatch() {
  if (!impl_->finished) {
    impl_->restoreSlots();
  }
}

StyleSystem::PreparedBatch::Worker* StyleSystem::PreparedBatch::bindWorker() {
  const std::lock_guard lock(impl_->workerMutex);
  const auto id = std::this_thread::get_id();
  const auto it = impl_->workers.find(id);
  if (it != impl_->workers.end()) {
    return it->second.get();
  }
  const auto bytes = impl_->reservationBytes();
  if (impl_->workers.size() == impl_->contexts || impl_->logical.rejected() ||
      !impl_->logical.retainedStyleBytes()->tryReservePreparation(bytes)) {
    impl_->abandoned.store(true, std::memory_order_relaxed);
    return nullptr;
  }
  auto worker = std::make_unique<Worker>(
      this, std::as_const(impl_->registry), impl_->logical, impl_->share,
      std::make_shared<PreparationLease>(impl_->logical.retainedStyleBytes(), bytes),
      impl_->enableFilter,
      static_cast<std::uint32_t>(impl_->workers.size() * StagedRecords::kMaximumRecords),
      impl_->layers.get());
  return impl_->workers.emplace(id, std::move(worker)).first->second.get();
}

bool StyleSystem::PreparedBatch::prepareDocumentRoot() {
  const Registry& view = impl_->registry;
  const auto* document = view.ctx().find<SVGDocumentContext>();
  if (!document || !view.valid(document->rootEntity)) {
    return true;
  }
  std::vector<Entity> chain;
  if (!impl_->traces.collectRootChain(view, document->rootEntity, chain)) {
    return false;
  }
  if (chain.empty()) {
    return true;
  }
  auto* worker = bindWorker();
  if (!worker) {
    return false;
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (!compute(*it, *worker)) {
      return false;
    }
  }
  return true;
}

bool StyleSystem::PreparedBatch::compute(Entity entity, Worker& worker) {
  UTILS_RELEASE_ASSERT(impl_->traces.entities().contains(entity));
  return computeAt(impl_->traces.entities().index(entity), entity, worker);
}

namespace {
bool HasShallowLocalStyle(const StyleComponent* local) {
  return !kNativeStyleStackAdmission || !local ||
         std::ranges::all_of(local->properties().unparsedProperties, [](const auto& item) {
           return RuleIndex::IsDeclarationShallowForParallelCascade(
               item.second.sourceDeclaration());
         });
}
}  // namespace

bool StyleSystem::PreparedBatch::computeAt(std::size_t index, Entity entity, Worker& worker,
                                           bool publishSlot) {
  UTILS_RELEASE_ASSERT(!impl_->finished && worker.owner_ == this);
  if (impl_->abandoned.load(std::memory_order_relaxed)) {
    return false;
  }
  const Registry& registry = impl_->registry;
  const auto* shadow = registry.try_get<ShadowEntityComponent>(entity);
  const auto* local = worker.localStyleForPreflight(registry, entity, shadow);
  if (!HasShallowLocalStyle(local)) {
    // Decline before copying a deep local CSS tree.
    impl_->abandoned.store(true, std::memory_order_relaxed);
    return false;
  }
  UTILS_RELEASE_ASSERT(index < impl_->traces.entries.size());
  // The constructor validated the slots before the workers received this immutable index.
  UTILS_RELEASE_ASSERT(impl_->traces.entities().data()[index] == entity);
  auto& trace = impl_->traces.entries[index];
  // The root prefix can reach an element before traversal visits it.
  if (trace.slot->style) {
    return true;
  }
  trace.publishSlot = publishSlot;
  if (worker.tryReuseStyle(registry, entity, shadow, local, trace)) {
    return true;
  }
  const auto firstWarning = worker.warnings_ ? worker.warnings_->count : 0;
  auto early = worker.sink(impl_->collectWarnings);
  auto late = worker.sink(impl_->collectWarnings);
  computePropertiesInto(std::as_const(impl_->registry), entity, *trace.slot, worker.pass_, early,
                        nullptr, &trace, &late);
  if (worker.budget_.rejected() || worker.records_.declined() ||
      !worker.keepWarnings(trace, firstWarning, early, late)) {
    trace.restoreOriginalSlot();
    impl_->abandoned.store(true, std::memory_order_relaxed);
    return false;
  }
  worker.rememberStyle(registry, entity, shadow, local, trace);
  return true;
}

std::size_t StyleSystem::PreparedBatch::commit(std::span<const Entity> requests,
                                               ParseWarningSink& warnings, bool useTraces) {
  UTILS_RELEASE_ASSERT(!impl_->finished);
  // Sharing one provisional record proves the workers compared the complete local key and
  // ordered rule list. A canonical hit additionally requires the rebound parent/root pair.
  // A provider can maintain additional work counters. The owner evaluates it in serial order.
  const bool replay = useTraces && !impl_->registry.ctx().contains<CascadeAncestorFilters>() &&
                      !impl_->abandoned.load(std::memory_order_relaxed);
  std::vector<RuleTrace::CacheHit> hits(
      replay ? impl_->workers.size() * StagedRecords::kMaximumRecords : 0);
  impl_->restoreSlots(hits);
  auto& counters = GetStyleCascadeMetrics(impl_->registry).counters;
  for (const auto& [id, worker] : impl_->workers) {
    counters.registryCopies += worker->counters_.registryCopies;
    counters.matchedPropertiesCacheHits += worker->counters_.matchedPropertiesCacheHits;
    counters.matchedPropertiesCacheMisses += worker->counters_.matchedPropertiesCacheMisses;
    counters.declarationParsesSkipped += worker->counters_.declarationParsesSkipped;
  }
  impl_->workers.clear();
  if (!replay) {
    for (auto& trace : impl_->traces.entries) {
      trace.diagnostics.reset();
    }
  }
  SerialCascade cascade(impl_->registry, warnings, replay ? &impl_->traces : nullptr,
                        impl_->enableFilter, true, &impl_->layers);
  for (Entity entity : requests) {
    cascade.compute(entity);
  }
  impl_->finished = true;
  impl_->traces.clear();
  return cascade.countRecordedWork(GetStyleCascadeMetrics(impl_->registry));
}

// All slots share the storage's ownership block. A cache eviction cannot uncharge a record
// still reachable from an element or from another worker's inherited cache key.
struct StyleSystem::StagedRecords::Storage {
  static constexpr std::size_t kPerPage = 16;
  using Page = std::array<std::optional<ComputedStyle>, kPerPage>;
  std::shared_ptr<const PreparationLease> lease;
  std::array<std::unique_ptr<Page>, kMaximumRecords / kPerPage> pages;
  std::size_t count = 0;
  std::size_t storageBytes = sizeof(Storage);
};

std::size_t StyleSystem::StagedRecords::retainedBytes() const {
  return storage_ ? storage_->storageBytes : 0;
}

namespace {

template <typename T>
bool HasBoundedStagedValue(const std::optional<T>& value) {
  if constexpr (std::is_same_v<T, PaintServer>) {
    return !value || std::visit(
                         [](const auto& paint) {
                           return std::is_trivially_copyable_v<std::decay_t<decltype(paint)>>;
                         },
                         value->value);
  } else if constexpr (std::is_same_v<T, StrokeDasharray>) {
    return true;
  } else {
    return !value || std::is_trivially_copyable_v<T>;
  }
}

std::optional<std::size_t> StagedPayloadBytes(const PropertyRegistry& properties) {
  if (!properties.unparsedProperties.empty() ||
      !std::apply(
          [](const auto&... property) { return (HasBoundedStagedValue(property.value) && ...); },
          properties.allProperties())) {
    return std::nullopt;
  }
  const auto& dashes = properties.strokeDasharray.value;
  if (!dashes) {
    return 0;
  }
  if (dashes->capacity() > StyleSystem::StagedRecords::kMaximumBytes / sizeof(Lengthd)) {
    return std::nullopt;
  }
  return dashes->capacity() * sizeof(Lengthd);
}

}  // namespace

std::shared_ptr<const ComputedStyle> StyleSystem::StagedRecords::retain(ComputedStyle& record) {
  static_assert(sizeof(Storage) + kMaximumRecords / Storage::kPerPage * sizeof(Storage::Page) <
                kMaximumBytes);
  const auto count = storage_ ? storage_->count : 0;
  const std::size_t available = kMaximumBytes - (storage_ ? retainedBytes() : sizeof(Storage));
  const std::size_t pageBytes = count % Storage::kPerPage == 0 ? sizeof(Storage::Page) : 0;
  const auto payload = StagedPayloadBytes(record.properties);
  if (count == kMaximumRecords || !payload || pageBytes > available ||
      *payload > available - pageBytes) {
    declined_ = true;
    return nullptr;
  }
  if (!storage_) {
    // Deallocate the record pages with their lease even if a weak observer survives.
    storage_ = std::shared_ptr<Storage>(new Storage());
    storage_->lease = lease_;
  }
  auto& page = storage_->pages[count / Storage::kPerPage];
  if (!page) {
    page = std::make_unique<Storage::Page>();
  }
  auto& staged = (*page)[count % Storage::kPerPage].emplace();
  staged.cachePlacement = firstRecord_ + static_cast<std::uint32_t>(count);
  staged.properties = std::move(record.properties);
  staged.customProperties = std::move(record.customProperties);
  staged.resourceOpacities = record.resourceOpacities;
  ++storage_->count;
  storage_->storageBytes += pageBytes + *payload;
  return std::shared_ptr<const ComputedStyle>(storage_, &staged);
}

const ComputedStyleComponent& StyleSystem::computeStyle(EntityHandle handle,
                                                        ParseWarningSink& warningSink) {
  if (auto* existing = handle.try_get<ComputedStyleComponent>(); existing && existing->style) {
    return *existing;
  }
  return SerialCascade(*handle.registry(), warningSink, nullptr, false).compute(handle.entity());
}

void StyleSystem::computePreparedStyle(const Registry& registry, Entity entity,
                                       ComputedStyleComponent& output, CascadePass& pass,
                                       ParseWarningSink& warnings) {
  UTILS_RELEASE_ASSERT_MSG(registry.try_get<ComputedStyleComponent>(entity) == &output,
                           "prepared output slot belongs to another entity");
  UTILS_RELEASE_ASSERT(!registry.ctx().contains<ExternalUseStyleContext>());
  UTILS_RELEASE_ASSERT(registry.view<const AnimatedValuesComponent>().empty());
  if (pass.staging && (pass.staging->declined() || pass.budget.rejected())) {
    pass.staging->declined_ = true;
    return;
  }
  computePropertiesInto(registry, entity, output, pass, warnings, nullptr);
  if (pass.staging && pass.budget.rejected()) {
    pass.staging->declined_ = true;
    output = {};
  }
}

bool StyleSystem::updateStyle(EntityHandle handle, std::string_view style,
                              bool invalidateCachedStyle) {
  auto& styleComponent = handle.get_or_emplace<StyleComponent>();
  StyleComponent candidate = styleComponent;

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
    return false;
  }
  const std::string mergedStyle =
      css::mergeStyleDeclarations(existingDeclarations, updateDeclarations);
  if (!candidate.setStyle(mergedStyle)) {
    return false;
  }

  attributes.setAttribute(*handle.registry(), xml::XMLQualifiedName("style"),
                          RcString(mergedStyle));
  styleComponent = std::move(candidate);

  if (invalidateCachedStyle) {
    invalidateComputed(handle);
  }
  return true;
}

std::size_t StyleSystem::CandidateListCapacityForTesting() {
  return CandidateList().capacity();
}

/// Hold only references while a cache hit is possible. Once the list is full, replay its
/// admitted prefix and parse later declarations directly into a heap-allocated record.
struct StyleSystem::PendingStyleDeclarations {
  struct Pending {
    const css::Declaration* declaration;
    css::Specificity specificity;
  };

  static std::shared_ptr<ComputedStyle> makeRecord(const StyleComponent* local) {
    return local ? std::make_shared<ComputedStyle>(local->properties())
                 : std::make_shared<ComputedStyle>();
  }

  std::shared_ptr<ComputedStyle> materialize(const StyleComponent* local, ParseWarningSink& sink) {
    if (streamed) {
      return std::move(streamed);
    }
    auto record = makeRecord(local);
    for (const auto& pending : entries) {
      ParseRuleDeclaration(*pending.declaration, pending.specificity, record->properties, sink);
    }
    return record;
  }

  std::shared_ptr<ComputedStyle> materializeWhenRequested(const StyleComponent* local,
                                                          ParseWarningSink& sink, bool defer) {
    return defer ? nullptr : materialize(local, sink);
  }

  std::shared_ptr<ComputedStyle> finishMaterialization(const StyleComponent* local,
                                                       ParseWarningSink& sink,
                                                       std::shared_ptr<ComputedStyle> record) {
    if (record) {
      return record;
    }
    const auto attempts = sink.warningAttempts();
    record = materialize(local, sink);
    UTILS_RELEASE_ASSERT(sink.warningAttempts() == attempts);
    return record;
  }

  void admit(const css::Declaration& declaration, css::Specificity specificity,
             const StyleComponent* local, ParseWarningSink& sink) {
    hasVariableInputs |= declaration.variableReferences == css::VariableReferences::Valid;
    constexpr std::size_t kMaximumDeferredDeclarations = 4096;
    if (!streamed && entries.size() == kMaximumDeferredDeclarations) {
      streamed = materialize(local, sink);
      entries.clear();
    }
    if (streamed) {
      ParseRuleDeclaration(declaration, specificity, streamed->properties, sink);
    } else {
      entries.push_back({&declaration, specificity});
    }
  }

  SmallVector<Pending, 4> entries;
  std::shared_ptr<ComputedStyle> streamed;
  bool hasVariableInputs = false;

  bool requiresPrivateResolution() const { return streamed || hasVariableInputs; }
};

void StyleSystem::applyStylesheetRules(const Registry& registry, Entity treeEntity,
                                       Entity dataEntity, StyleResourceBudget& styleBudget,
                                       StyleCascadeCounters& counters,
                                       MatchedPropertiesKey& cacheKey, const StyleComponent* local,
                                       ParseWarningSink& warningSink,
                                       PendingStyleDeclarations& declarations, RuleTrace* capture,
                                       RuleTrace* replay, StyleAncestorFilter* ancestorFilter,
                                       CascadeAncestorFilter* testFilter,
                                       const DocumentLayerOrder* layers) {
  const auto admitMatch = [&](const RuleTrace::Match& match) {
    ++counters.rulesMatched;
    cacheKey.addRule(match.revision, match.ordinal, match.position, match.specificity);
    AdmitRuleDeclarations(*match.rule, match.specificity, styleBudget, counters,
                          [&](const css::Declaration& declaration, css::Specificity specificity) {
                            declarations.admit(declaration, specificity, local, warningSink);
                          });
  };
  if (RuleTrace::reuseSelectorWork(capture, replay, treeEntity, styleBudget, counters,
                                   ancestorFilter, admitMatch)) {
    return;
  }
  AncestorFilterCheck filterCheck(ancestorFilter, treeEntity, counters);
  const RuleTrace::Capture recording(capture, styleBudget, filterCheck, counters);
  const auto [subjectOnly, tagOnly] = RuleTrace::observation(capture);
  const ShadowedElementAdapter adapter(registry, treeEntity, dataEntity, subjectOnly, tagOnly);
  const RuleIndexElementView elementKeys(registry, treeEntity, dataEntity, tagOnly);
  const bool unindexed = registry.ctx().contains<UnindexedCascadeForTesting>();
  CandidateListScope candidateScope;
  std::vector<std::uint32_t>& candidates = candidateScope.list();
  // Apply one sheet's candidate rules in rule order; returns false once the style budget is
  // exhausted. Rules that cannot match are not candidates and cost nothing.
  const auto applyRules = [&](std::uint64_t sheetRevision, std::size_t sheetOrdinal,
                              const css::Stylesheet& sheet, const RuleIndex& index,
                              bool isUserAgentStylesheet) {
    const RuleLayerRanks ranks(layers, isUserAgentStylesheet, sheet);
    const std::span<const css::SelectorRule> rules = sheet.rules();
    CollectCandidateRules(rules, index, elementKeys, unindexed, candidates);
    for (const std::uint32_t position : candidates) {
      const css::SelectorRule& rule = rules[position];
      const auto rank = ranks.rank(rule.layer);
      if (!rank) {
        continue;
      }
      if (!styleBudget.reserveRuleElementMatch()) {
        return false;
      }
      ++counters.rulesTested;
      const auto match = filterCheck.evaluate(index, position, [&] {
        return MatchCascadeCandidate(rule, adapter, styleBudget.selectorTraversal(), testFilter,
                                     index, position, unindexed);
      });
      if (!match) {
        continue;
      }
      const auto specificity =
          CascadeSpecificity(match->specificity.withLayerRank(*rank), isUserAgentStylesheet);
      const RuleTrace::Match matched{sheetRevision, sheetOrdinal, position, &rule, specificity};
      recording.append(matched);
      admitMatch(matched);
    }
    return true;
  };

  const auto applyStylesheet = [&](const StylesheetComponent& stylesheet, Entity stylesheetEntity) {
    if (!IsApplicableStylesheet(registry, stylesheet, stylesheetEntity, dataEntity)) {
      return true;
    }
    if (!VisitCascadeImports(stylesheet, layers, applyRules)) {
      return false;
    }
    return applyRules(stylesheet.contentRevision(), stylesheet.importedStylesheets.size(),
                      stylesheet.stylesheet, stylesheet.ruleIndex,
                      stylesheet.isUserAgentStylesheet);
  };
  VisitCascadeStylesheets(registry, layers, applyStylesheet);
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
  return stroke ? style.properties()->stroke.get() : style.properties()->fill.get();
}

void StyleSystem::inheritComputedParent(const Registry& registry, Entity entity, Entity parent,
                                        PropertyRegistry& properties,
                                        ComputedStyleComponent& computed,
                                        ParseWarningSink& warningSink, SerialCascade* serial) {
  const auto* occurrence = ExternalRootContext(registry, entity);
  const PropertyState fillState = properties.fill.state;
  const PropertyState strokeState = properties.stroke.state;
  if (occurrence) {
    const LocalResourceStates local(properties);
    inheritExternalParent(properties, *occurrence->parent.properties());
    if (BlockExternalResources(properties, local)) {
      UTILS_RELEASE_ASSERT(serial);
      serial->rejectExternalResource();
      warningSink.add(ParseDiagnostic::Error("External-use inherited URL resources are unsupported",
                                             FileOffset::Offset(0)));
    }
  } else {
    inheritProperties(registry, parent, properties);
  }
  const auto* parentStyle =
      parent != entt::null ? registry.try_get<ComputedStyleComponent>(parent) : nullptr;
  const bool allowPaint = CanInheritPaint(registry, parent, occurrence);
  computed.externalFill =
      ExternalPaintOrigin(fillState, occurrence != nullptr, parentStyle, false, allowPaint);
  computed.externalStroke =
      ExternalPaintOrigin(strokeState, occurrence != nullptr, parentStyle, true, allowPaint);
}

void StyleSystem::resolveComputedParentFonts(const Registry& registry, Entity entity, Entity parent,
                                             PropertyRegistry& properties) {
  if (const auto* occurrence = ExternalRootContext(registry, entity)) {
    const PropertyRegistry& host = *occurrence->parent.properties();
    properties.resolveFontSize(host.fontSize.get().value().value);
    properties.resolveFontWeight(host.fontWeight.get().value());
    properties.resolveFontStretch(host.fontStretch.get().value());
  } else {
    resolveRelativeFontProperties(registry, parent, properties);
  }
}

void StyleSystem::inheritExternalParent(PropertyRegistry& properties,
                                        const PropertyRegistry& parent) {
  auto destination = properties.allPropertiesMutable();
  const auto inherited = parent.allProperties();
  [&]<std::size_t... index>(std::index_sequence<index...>) {
    (InheritExternalProperty(std::get<index>(destination), std::get<index>(inherited)), ...);
  }(std::make_index_sequence<PropertyRegistry::numProperties()>{});
}

void StyleSystem::inheritProperties(const Registry& registry, Entity parent,
                                    PropertyRegistry& properties) {
  if (parent == entt::null) {
    return;
  }
  const auto& parentStyleComponent = registry.get<ComputedStyleComponent>(parent);
  const PropertyInheritOptions inheritOptions = registry.all_of<DoNotInheritFillOrStrokeTag>(parent)
                                                    ? PropertyInheritOptions::NoPaint
                                                    : PropertyInheritOptions::All;
  InheritPreservingLocalValues(properties, *parentStyleComponent.properties(), inheritOptions);
}

void StyleSystem::resolveRelativeFontProperties(const Registry& registry, Entity parent,
                                                PropertyRegistry& properties) {
  double parentFontSizePx = 12.0;
  int parentFontWeight = 400;
  int parentFontStretch = static_cast<int>(FontStretch::Normal);
  if (parent != entt::null) {
    if (const PropertyRegistry* parentProperties =
            registry.get<ComputedStyleComponent>(parent).properties()) {
      parentFontSizePx = parentProperties->fontSize.get().value().value;
      parentFontWeight = parentProperties->fontWeight.get().value();
      parentFontStretch = parentProperties->fontStretch.get().value();
    }
  }
  properties.resolveFontSize(parentFontSizePx);
  properties.resolveFontWeight(parentFontWeight);
  properties.resolveFontStretch(parentFontStretch);
}

[[gnu::always_inline]] inline bool StyleSystem::computeCustomProperties(
    ComputedStyle& record, const ComputedStyleComponent* parent, CascadePass& pass,
    ParseWarningSink& warnings) {
  if (parent && parent->style && parent->style->customProperties) {
    record.customProperties = parent->style->customProperties;
  }
  if (!HasLocalCustomProperties(record.properties)) {
    return true;
  }
  if (pass.staging) {
    // Retained maps and token sequences are charged in canonical document order.
    pass.staging->declined_ = true;
    return false;
  }
  ResolveCustomProperties(record, pass, warnings);
  return true;
}

[[gnu::always_inline]] inline bool StyleSystem::resolveComputedVariables(
    ComputedStyle& record, const ComputedStyleComponent* parent, CascadePass& pass,
    ParseWarningSink& warnings) {
  return computeCustomProperties(record, parent, pass, warnings) &&
         (!record.properties.mayHaveVariableInputs() ||
          resolvePendingVariables(record, pass, warnings));
}

[[gnu::noinline]] bool StyleSystem::resolvePendingVariables(ComputedStyle& record,
                                                            CascadePass& pass,
                                                            ParseWarningSink& warnings) {
  if (!record.properties.hasPendingVariableDeclarations()) {
    return true;
  }
  if (pass.staging) {
    // Generated work must use the canonical owner's declaration budget.
    pass.staging->declined_ = true;
    return false;
  }
  const auto exhaustedAt = record.properties.resolveVariableDeclarations(
      [&](std::string_view name) -> std::optional<std::span<const css::ComponentValue>> {
        const auto* sequence =
            record.customProperties ? record.customProperties->find(name) : nullptr;
        return sequence ? std::optional(sequence->tokens()) : std::nullopt;
      },
      [&](std::size_t count, std::size_t bytes) {
        return pass.budget.reserveGeneratedDeclarationWork(count, bytes);
      });
  if (exhaustedAt && pass.budget.reportVariableLimitWarning()) {
    warnings.add(ParseDiagnostic::Warning("CSS variable expansion exceeds the style work limit",
                                          *exhaustedAt));
  }
  return true;
}

namespace {

bool HasInlineVariableInputs(const StyleComponent* local) {
  return local && local->properties().mayHaveVariableInputs() &&
         std::ranges::any_of(local->properties().unparsedProperties, [](const auto& item) {
           return item.second.sourceDeclaration().variableReferences ==
                      css::VariableReferences::Valid ||
                  item.second.presentationVariable;
         });
}

bool CanShareDeclarationValues(bool streamedOrVariable, const StyleComponent* local) {
  return !streamedOrVariable && !HasInlineVariableInputs(local);
}

}  // namespace

void StyleSystem::computePropertiesInto(const Registry& registry, Entity entity,
                                        ComputedStyleComponent& computedStyle, CascadePass& pass,
                                        ParseWarningSink& warningSink, SerialCascade* serial,
                                        RuleTrace* capture, ParseWarningSink* workerAfter,
                                        RuleTrace* replay) {
  if (computedStyle.style) {
    return;  // Already computed.
  }

  const entt::basic_handle<const Registry> handle(registry, entity);
  auto& styleBudget = pass.budget;
  const RegistryCopyCount copyCount(pass);
  auto& counters = pass.counters;
  ++counters.elementsCascaded;

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();
  const Entity parent = handle.get<donner::components::TreeComponent>().parent();
  const auto* occurrence = ExternalRootContext(registry, handle.entity());
  const auto* styleComponent = registry.try_get<StyleComponent>(dataEntity);
  auto diagnostics = RuleTrace::diagnosticsFor(replay, warningSink, workerAfter);
  auto& early = diagnostics.before();
  const std::size_t warningAttemptsBefore = early.warningAttempts();

  MatchedPropertiesKey cacheKey;
  PendingStyleDeclarations declarations;
  applyStylesheetRules(registry, handle.entity(), dataEntity, styleBudget, counters, cacheKey,
                       styleComponent, early, declarations, capture, replay, pass.ancestorFilter,
                       pass.testFilter, pass.layers);

  PopulateMatchedPropertiesKey(registry, handle, dataEntity, parent, styleComponent, cacheKey);
  MatchedCacheState cacheState = PrepareMatchedCacheState(
      registry, dataEntity, parent, occurrence,
      !CanShareDeclarationValues(declarations.requiresPrivateResolution(), styleComponent),
      cacheKey, styleBudget);
  if (!serial) {
    RuleTrace::bindPreparedInputs(capture, registry, parent, cacheKey, cacheState);
  }
  if (TryInstallMatchedStyle(registry, pass.cache, parent, cacheKey, declarations.entries.size(),
                             styleBudget, counters, computedStyle, cacheState)) {
    RuleTrace::rememberCacheOutcome(capture, replay, pass.cache);
    return;
  }

  // A complete warning-free trace has already parsed these immutable inputs, or found
  // a worker entry whose original parse produced no warnings. With an unpublished parent,
  // defer the redundant local parse until after the ordinary late cache probe.
  const bool deferMaterialization = RuleTrace::mayDeferMaterialization(replay, cacheState);
  auto record = declarations.materializeWhenRequested(styleComponent, early, deferMaterialization);
  auto* animationBudget = serial ? serial->animationBudget() : nullptr;
  const bool warningBeforeParent = early.warningAttempts() != warningAttemptsBefore;
  diagnostics.beforeParent();
  SerialCascade::publishMissingParent(serial, parent, cacheState);
  auto& late = diagnostics.after();
  const std::size_t warningAttemptsAfterParent = late.warningAttempts();
  if (TryInstallLateMatchedStyle(registry, pass.cache, parent, cacheKey, warningBeforeParent,
                                 styleBudget, counters, computedStyle, cacheState)) {
    RuleTrace::rememberCacheOutcome(capture, replay, pass.cache);
    return;
  }
  record = declarations.finishMaterialization(styleComponent, early, std::move(record));
  if (!resolveComputedVariables(*record, cacheState.parentStyle, pass, late)) {
    return;
  }
  PropertyRegistry& properties = record->properties;
  ApplyAnimatedStyle(registry, dataEntity, properties);
  ApplyContextualFontSize(registry, animationBudget, dataEntity, cacheState.parentStyle,
                          properties);
  const auto resourceOpacities = ApplyScalarAnimationStyles(
      registry, animationBudget, dataEntity, cacheState.parentStyle, properties, styleBudget, late);
  ApplySymbolInstanceDisplay(handle, dataEntity, properties);
  properties.resolveElementColors(late);
  const bool inheritPaint = CanInheritPaint(registry, parent, occurrence);
  ApplyPaintAnimationStyles(registry, animationBudget, dataEntity, cacheState.parentStyle,
                            inheritPaint, properties);
  const PropertyState fillState = properties.fill.state;
  const PropertyState strokeState = properties.stroke.state;

  // Inherit from parent, then charge the dynamic bytes retained by the final computed style.
  // Charging the parent's source value alone misses large locally parsed vectors and can also
  // differ from the actual value selected by the cascade.
  inheritComputedParent(registry, entity, parent, properties, computedStyle, late, serial);

  record->cachePlacement = static_cast<std::uint32_t>(entity);
  RetainComputedRecord(pass, *record, computedStyle, resourceOpacities);

  // Resolve font-size to absolute pixels. CSS spec requires the computed value of font-size to
  // always be an absolute length. Relative units (em, %, ex) resolve against the parent's computed
  // font-size, and percentages resolve against the parent font-size (not the viewBox). Local
  // text-indent em lengths compute from that result before descendants inherit them.
  resolveComputedParentFonts(registry, entity, parent, properties);
  const double rootFontSizePx = ComputedRootFontSize(registry, handle.entity(), properties);
  properties.resolveTextDecorationThickness(rootFontSizePx);
  properties.resolveInsetClipLengths(rootFontSizePx);
  computedStyle.style = pass.staging ? pass.staging->retain(*record) : std::move(record);
  if (!computedStyle.style) {
    computedStyle = {};
    return;
  }

  if (FinishMatchedCacheMiss(registry, pass.cache, cacheKey, fillState, strokeState,
                             warningBeforeParent, warningAttemptsAfterParent, late, styleBudget,
                             counters, computedStyle, cacheState)) {
    RuleTrace::rememberCacheOutcome(capture, replay, pass.cache);
  }
}

namespace {

MatchedStyleRule BuildMatchedStyleRule(const StylesheetComponent& stylesheet,
                                       Entity stylesheetEntity, const css::SelectorRule& rule,
                                       std::size_t ruleIndex, std::size_t selectorEntryIndex,
                                       css::Specificity specificity) {
  MatchedStyleRule matchedRule;
  matchedRule.stylesheetEntity = stylesheetEntity;
  matchedRule.ruleIndex = ruleIndex;
  matchedRule.selectorEntryIndex = selectorEntryIndex;
  matchedRule.specificity = specificity;
  matchedRule.isUserAgentStylesheet = stylesheet.isUserAgentStylesheet;
  matchedRule.ruleSourceRange = stylesheet.sourceMap.mapToDocumentSource(rule.ruleSourceRange);

  if (selectorEntryIndex < rule.selectorEntrySourceRanges.size()) {
    matchedRule.selectorSourceRange = stylesheet.sourceMap.mapToDocumentSource(
        rule.selectorEntrySourceRanges[selectorEntryIndex]);
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

  return matchedRule;
}

}  // namespace

std::vector<MatchedStyleRule> StyleSystem::collectMatchedStyleRules(EntityHandle handle) const {
  Registry& registry = *handle.registry();

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();

  std::vector<MatchedStyleRule> result;
  StyleResourceBudget styleBudget;
  const ShadowedElementAdapter adapter(registry, handle.entity(), dataEntity);
  const RuleIndexElementView elementKeys(registry, handle.entity(), dataEntity);
  const bool unindexed = registry.ctx().contains<UnindexedCascadeForTesting>();
  std::vector<std::uint32_t> candidates;
  const auto layers = DocumentLayerOrder::Build(registry);
  const auto collect = [&](const StylesheetComponent& stylesheet, Entity stylesheetEntity) {
    if (!IsApplicableStylesheet(registry, stylesheet, stylesheetEntity, dataEntity)) {
      return true;
    }
    const RuleLayerRanks ranks(layers.get(), stylesheet.isUserAgentStylesheet,
                               stylesheet.stylesheet);
    const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();
    CollectCandidateRules(rules, stylesheet.ruleIndex, elementKeys, unindexed, candidates);

    for (const std::uint32_t ruleIndex : candidates) {
      const css::SelectorRule& rule = rules[ruleIndex];
      const auto rank = ranks.rank(rule.layer);
      if (!rank) {
        continue;
      }
      if (!styleBudget.reserveRuleElementMatch()) {
        return false;
      }
      std::optional<MatchedSelectorEntry> match =
          MatchSelectorRule(rule, adapter, &styleBudget.selectorTraversal());
      if (!match.has_value()) {
        continue;
      }

      const auto specificity = CascadeSpecificity(match->specificity.withLayerRank(*rank),
                                                  stylesheet.isUserAgentStylesheet);

      result.push_back(BuildMatchedStyleRule(stylesheet, stylesheetEntity, rule, ruleIndex,
                                             match->selectorEntryIndex, specificity));
    }
    return true;
  };
  VisitCascadeStylesheets(registry, layers.get(), collect);

  return result;
}

namespace {

struct CustomPropertyWinner {
  const css::Declaration* declaration = nullptr;
  css::Specificity specificity;
  // Two null entities represent a source-less rule.
  Entity inlineEntity = entt::null;
  Entity sheetEntity = entt::null;
};

void ConsiderCustomProperty(CustomPropertyWinner& winner, std::string_view name,
                            const css::Declaration& declaration, css::Specificity priority,
                            Entity sheetEntity) {
  if (std::string_view(declaration.name) != name ||
      declaration.variableReferences == css::VariableReferences::Invalid) {
    return;
  }
  if (declaration.important) {
    priority = css::Specificity::Important(priority);
  }
  if (!winner.declaration || winner.specificity <= priority) {
    winner = {&declaration, priority, entt::null, sheetEntity};
  }
}

struct CustomPropertySearch {
  const ShadowedElementAdapter& adapter;
  const RuleIndexElementView& keys;
  const DocumentLayerOrder* layers;
  StyleResourceBudget& budget;
  bool unindexed;
  std::string_view name;
  CustomPropertyWinner winner;
  std::vector<std::uint32_t> candidates;
  StyleCascadeCounters counters;

  bool visit(const css::Stylesheet& sheet, const RuleIndex& index, bool userAgent,
             Entity sourceEntity) {
    const RuleLayerRanks ranks(layers, userAgent, sheet);
    const auto rules = sheet.rules();
    CollectCandidateRules(rules, index, keys, unindexed, candidates);
    for (const auto position : candidates) {
      const auto& rule = rules[position];
      const auto rank = ranks.rank(rule.layer);
      if (!rank) {
        continue;
      }
      if (!budget.reserveRuleElementMatch()) {
        return false;
      }
      const auto match = MatchCascadeCandidate(rule, adapter, budget.selectorTraversal(), nullptr,
                                               index, position, unindexed);
      if (!match) {
        continue;
      }
      const auto priority = CascadeSpecificity(match->specificity.withLayerRank(*rank), userAgent);
      AdmitRuleDeclarations(rule, priority, budget, counters,
                            [&](const css::Declaration& declaration, css::Specificity specificity) {
                              ConsiderCustomProperty(winner, name, declaration, specificity,
                                                     sourceEntity);
                            });
    }
    return !budget.rejected();
  }
};

CustomPropertyWinner FindLocalCustomProperty(const Registry& registry, Entity treeEntity,
                                             Entity dataEntity, std::string_view name,
                                             const DocumentLayerOrder* layers,
                                             StyleResourceBudget& budget) {
  const ShadowedElementAdapter adapter(registry, treeEntity, dataEntity);
  const RuleIndexElementView keys(registry, treeEntity, dataEntity);
  CustomPropertySearch search{
      adapter, keys, layers, budget, registry.ctx().contains<UnindexedCascadeForTesting>(),
      name,    {},   {},     {}};
  if (const auto* style = registry.try_get<StyleComponent>(dataEntity)) {
    const auto& properties = style->properties().unparsedProperties;
    if (const auto local = properties.find(RcString(name)); local != properties.end()) {
      search.winner = {&local->second.declaration, local->second.specificity, dataEntity};
    }
  }
  VisitCascadeStylesheets(
      registry, layers, [&](const StylesheetComponent& stylesheet, Entity sheetEntity) {
        if (!IsApplicableStylesheet(registry, stylesheet, sheetEntity, dataEntity)) {
          return true;
        }
        const auto imported = [&](std::uint64_t, std::size_t, const css::Stylesheet& sheet,
                                  const RuleIndex& index, bool userAgent) {
          return search.visit(sheet, index, userAgent, entt::null);
        };
        return VisitCascadeImports(stylesheet, layers, imported) &&
               search.visit(stylesheet.stylesheet, stylesheet.ruleIndex,
                            stylesheet.isUserAgentStylesheet,
                            stylesheet.isUserAgentStylesheet ? entt::null : sheetEntity);
      });
  return search.winner;
}

std::optional<SourceRange> MapStylesheetText(const xml::XMLNode& node, std::string_view css,
                                             std::size_t start, std::size_t end) {
  // Recover the exact XML text chunk for the CSS token. A token split between chunks
  // has no contiguous document range.
  std::size_t offset = 0;
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() != xml::XMLNode::Type::Data && child->type() != xml::XMLNode::Type::CData) {
      continue;
    }
    const auto chunk = child->value();
    if (!chunk) {
      continue;
    }
    if (start >= offset && end - offset <= chunk->size()) {
      if (css.substr(offset, chunk->size()) != std::string_view(*chunk)) {
        return std::nullopt;
      }
      return child->mapDecodedTextRange(
          {FileOffset::Offset(start - offset), FileOffset::Offset(end - offset)});
    }
    offset += chunk->size();
  }
  return offset == 0
             ? node.mapProjectedTextRange(css, {FileOffset::Offset(start), FileOffset::Offset(end)})
             : std::nullopt;
}

bool HasCurrentSourceProjection(const Registry& registry) {
  const auto* document = registry.ctx().find<xml::components::XMLDocumentContext>();
  return document && document->sourceStoreHolder->store &&
         !document->sourceRequiresDocumentReparse && document->unreparsedSpans.empty();
}

std::optional<SourceRange> MapEmbeddedStylesheetRange(Registry& registry, Entity stylesheetEntity,
                                                      const SourceRange& cssRange) {
  const auto* sheet = registry.try_get<StylesheetComponent>(stylesheetEntity);
  if (!sheet || sheet->isUserAgentStylesheet) {
    return std::nullopt;
  }
  const auto start = cssRange.start.offset;
  const auto end = cssRange.end.offset;
  if (!start || !end || *end <= *start || *end > sheet->text.size()) {
    return std::nullopt;
  }
  const auto node = xml::XMLNode::TryCast(EntityHandle(registry, stylesheetEntity));
  return node ? MapStylesheetText(*node, sheet->text, *start, *end) : std::nullopt;
}

std::optional<SourceRange> MapCustomPropertyWinner(Registry& registry,
                                                   const CustomPropertyWinner& winner) {
  const auto start = winner.declaration->sourceOffset.offset;
  const std::size_t length = winner.declaration->nameSourceByteLength;
  if (!start || length == 0 || length > std::numeric_limits<std::size_t>::max() - *start) {
    return std::nullopt;
  }
  const auto end = *start + length;
  const SourceRange local{FileOffset::Offset(*start), FileOffset::Offset(end)};
  if (winner.inlineEntity != entt::null) {
    const auto node = xml::XMLNode::TryCast(EntityHandle(registry, winner.inlineEntity));
    return node ? node->mapDecodedAttributeRange(xml::XMLQualifiedNameRef("style"), local)
                : std::nullopt;
  }
  return winner.sheetEntity != entt::null
             ? MapEmbeddedStylesheetRange(registry, winner.sheetEntity, local)
             : std::nullopt;
}

bool CanTraceCustomProperty(const Registry& registry, EntityHandle handle, std::string_view name) {
  if (!HasCurrentSourceProjection(registry)) {
    return false;
  }
  const auto* budget = registry.ctx().find<StyleResourceBudget>();
  const auto* computed = handle.try_get<ComputedStyleComponent>();
  return !(budget && budget->rejected()) && computed && computed->style &&
         computed->style->customProperties && computed->style->customProperties->find(name);
}

}  // namespace

std::optional<SourceRange> StyleSystem::mapStylesheetSourceRange(
    Registry& registry, Entity stylesheetEntity, const SourceRange& cssRange) const {
  if (!HasCurrentSourceProjection(registry) || !registry.valid(stylesheetEntity)) {
    return std::nullopt;
  }
  return MapEmbeddedStylesheetRange(registry, stylesheetEntity, cssRange);
}

std::optional<SourceRange> StyleSystem::findCustomPropertyDeclaration(EntityHandle handle,
                                                                      std::string_view name) const {
  Registry& registry = *handle.registry();
  if (!CanTraceCustomProperty(registry, handle, name)) {
    return std::nullopt;
  }
  const auto layers = DocumentLayerOrder::Build(registry);
  StyleResourceBudget budget;
  for (Entity current = handle.entity(); current != entt::null;) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      return std::nullopt;
    }
    const auto* shadow = registry.try_get<ShadowEntityComponent>(current);
    const Entity dataEntity = shadow ? shadow->lightEntity : current;
    const auto winner =
        FindLocalCustomProperty(registry, current, dataEntity, name, layers.get(), budget);
    if (budget.rejected()) {
      return std::nullopt;
    }
    using Action = CustomPropertyMap::Action;
    const auto action =
        winner.declaration ? CustomPropertyAction(*winner.declaration) : Action::Inherit;
    if (action == Action::Set) {
      return MapCustomPropertyWinner(registry, winner);
    }
    // An explicit initial value cannot supply the selected computed token sequence.
    if (action == Action::Initial) {
      return std::nullopt;
    }
    if (ExternalRootContext(registry, current)) {
      return std::nullopt;
    }
    current = tree->parent();
  }
  return std::nullopt;
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

namespace {

bool HasPotentialLayeredFonts(const StylesheetComponent& sheet) {
  if (!sheet.isCssType() || sheet.isUserAgentStylesheet || sheet.cascadeFontFaces().empty()) {
    return false;
  }
  return sheet.stylesheet.layers() ||
         std::ranges::any_of(sheet.importedStylesheets,
                             [](const auto& imported) { return imported.layers(); });
}

class StylesheetFontOrder {
public:
  explicit StylesheetFontOrder(ResourceManagerContext& resources) : resources_(resources) {}

  void add(const css::Stylesheet& sheet, const DocumentLayerOrder* layers) {
    const auto faces = sheet.fontFaces();
    const auto ranks = layers ? layers->ranksFor(sheet) : DocumentLayerOrder::SheetRanks{};
    for (std::size_t i = 0; i < faces.size(); ++i) {
      const std::uint16_t rank = layers ? ranks.forLayer(sheet.fontFaceLayer(i)) : 1;
      if (rank != 0) {
        addFace(faces[i], rank);
      }
    }
  }

  std::vector<std::size_t> ordered() const {
    std::vector<std::pair<Priority, std::size_t>> sorted;
    sorted.reserve(priorities_.size());
    for (const auto& [index, priority] : priorities_) {
      sorted.emplace_back(priority, index);
    }
    std::ranges::sort(sorted);
    std::vector<std::size_t> result;
    result.reserve(sorted.size());
    for (const auto& [_, index] : sorted) {
      result.push_back(index);
    }
    return result;
  }

private:
  using Priority = std::pair<std::uint16_t, std::size_t>;

  void addFace(const css::FontFace& face, std::uint16_t rank) {
    if (const auto index = resources_.registerStylesheetFontFace(face)) {
      const Priority priority(rank, next_++);
      auto [current, inserted] = priorities_.try_emplace(*index, priority);
      if (!inserted) {
        current->second = std::max(current->second, priority);
      }
    }
  }

  ResourceManagerContext& resources_;
  std::unordered_map<std::size_t, Priority> priorities_;
  std::size_t next_ = 0;
};

void AddStylesheetFontLayers(StylesheetFontOrder& order, const StylesheetComponent& sheet,
                             const DocumentLayerOrder* layers) {
  if (!layers || layers->allowsImportsOf(sheet.stylesheet)) {
    for (const auto& imported : sheet.importedStylesheets) {
      order.add(imported, layers);
    }
  }
  order.add(sheet.stylesheet, layers);
}

void UpdateLayeredFontOrder(Registry& registry, ResourceManagerContext& resources) {
  const auto* document = registry.ctx().find<SVGDocumentContext>();
  const auto revision = document ? std::optional(document->mutationRevision()) : std::nullopt;
  if (revision && resources.hasLayeredFontOrderFor(*revision)) {
    return;
  }
  const auto layers = DocumentLayerOrder::Build(registry);
  const auto* activeLayers = layers && layers->hasRanks() ? layers.get() : nullptr;
  StylesheetFontOrder order(resources);
  const auto add = [&](Entity entity) {
    const auto& sheet = registry.get<StylesheetComponent>(entity);
    if (sheet.isCssType() && HasSvgNamespace(registry, entity)) {
      AddStylesheetFontLayers(order, sheet, activeLayers);
    }
  };
  if (activeLayers) {
    std::ranges::for_each(activeLayers->stylesheets(), add);
  } else {
    std::ranges::for_each(DocumentLayerOrder::StylesheetsInDocumentOrder(registry), add);
  }
  resources.setLayeredStylesheetFontOrder(order.ordered(), revision);
}

}  // namespace

void StyleSystem::synchronizeDocumentFontFaces(Registry& registry) {
  auto& resources = registry.ctx().get<ResourceManagerContext>();
  const auto view = registry.view<const StylesheetComponent>();
  const auto usesLayers = [&](Entity entity) {
    return HasPotentialLayeredFonts(view.get<const StylesheetComponent>(entity));
  };
  if (resources.layeredFontFaceOrder() || std::ranges::any_of(view, usesLayers)) {
    UpdateLayeredFontOrder(registry, resources);
    return;
  }
  for (Entity entity : view) {
    SynchronizeStylesheetFonts(resources, registry, entity,
                               view.get<const StylesheetComponent>(entity));
  }
}

void StyleSystem::computeAllStyles(Registry& registry, ParseWarningSink& warningSink,
                                   FullRestyle fullRestyle, StylePassScheduler scheduler) {
  StyleCascadeMetrics& metrics = GetStyleCascadeMetrics(registry);
  const StyleTimeScope timeScope(metrics);
  auto& styleBudget = GetStyleResourceBudget(registry);
  styleBudget.reset();
  const PassRejectionCount rejectionCount(styleBudget, metrics.counters);
  const auto* renderState = registry.ctx().find<RenderTreeState>();
  const bool hasBeenBuilt = renderState != nullptr && renderState->hasBeenBuilt;
  const bool needsFullStyleRecompute =
      renderState != nullptr && renderState->needsFullStyleRecompute;
  const bool hasDirtyEntities = !registry.view<DirtyFlagsComponent>().empty();

  // Selective pass: recompute only the entities flagged dirty. This depends on a two-part
  // invariant that mutation hooks must uphold together, because either half alone is inert:
  //
  //   1. The entity's cached ComputedStyleComponent must have been removed. computeStyle() is
  //      memoized (see computePropertiesInto, which returns early once `style` is set), so
  //      a Style dirty flag on an entity whose cached style is still populated does nothing.
  //   2. The entity must be flagged DirtyFlagsComponent::Style. Removing the cached style without
  //      flagging it leaves the entity with no computed style at all, which later passes that
  //      walk ComputedStyleComponent would then skip or dereference.
  //
  // Writers that cannot bound the affected entities, such as stylesheet or structural edits,
  // instead request RenderTreeState::needsFullStyleRecompute.
  if (hasBeenBuilt && hasDirtyEntities && !needsFullStyleRecompute) {
    computeDirtyStyles(registry, warningSink, scheduler);
  } else {
    if (hasBeenBuilt && needsFullStyleRecompute &&
        fullRestyle == FullRestyle::DiscardAndRecompute) {
      discardComputedStylesForFullRestyle(registry);
    }
    computeMissingStyles(registry, warningSink, scheduler);
  }

  synchronizeDocumentFontFaces(registry);
}

void StyleSystem::computeDirtyStyles(Registry& registry, ParseWarningSink& warningSink,
                                     StylePassScheduler scheduler) {
  if (scheduler.tryCompute(registry, warningSink, StylePassSelection::Dirty)) {
    return;
  }
  SerialCascade cascade(registry, warningSink, nullptr, true, true);
  for (auto entity : registry.view<DirtyFlagsComponent>()) {
    if (!registry.get<DirtyFlagsComponent>(entity).test(DirtyFlagsComponent::Style)) {
      continue;
    }

    cascade.compute(entity);
  }
}

void StyleSystem::computeMissingStyles(Registry& registry, ParseWarningSink& warningSink,
                                       StylePassScheduler scheduler) {
  // Create placeholder ComputedStyleComponents for all elements in the range, since creating
  // computed style components also creates the parents, and we can't modify the component list
  // while iterating it.
  auto view = registry.view<donner::components::TreeComponent>();
  for (auto entity : view) {
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }

  if (scheduler.tryCompute(registry, warningSink, StylePassSelection::Missing)) {
    return;
  }
  SerialCascade cascade(registry, warningSink, nullptr, true, true);
  // Compute the styles for all elements; computeStyle returns early for computed ones.
  for (auto entity : view) {
    cascade.compute(entity);
  }
}

void StyleSystem::discardComputedStyles(Registry& registry) {
  MatchedPropertiesCache::discardComputedStyles(registry);
}

void StyleSystem::discardComputedStylesForFullRestyle(Registry& registry) {
  StyleCascadeMetrics& metrics = GetStyleCascadeMetrics(registry);
  const StyleTimeScope timeScope(metrics);
  ++metrics.counters.fullClears;
  discardComputedStyles(registry);
}

void StyleSystem::computeStylesFor(Registry& registry, std::span<const Entity> entities,
                                   ParseWarningSink& warningSink) {
  SerialCascade cascade(registry, warningSink, nullptr, true, true);
  const StyleTimeScope timeScope(GetStyleCascadeMetrics(registry));
  for (Entity entity : entities) {
    cascade.compute(entity);
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

namespace {

bool RequiresFullStyleBudgetReplay(const Registry& registry) {
  const auto* budget = registry.ctx().find<StyleResourceBudget>();
  const auto* metrics = registry.ctx().find<StyleCascadeMetrics>();
  return (budget && (budget->rejected() || budget->limits() != StyleResourceBudget::Limits())) ||
         (metrics && metrics->counters.budgetRejections != 0);
}

}  // namespace

bool StyleSystem::computeEditedFrameStyles(Registry& registry, ParseWarningSink& warnings,
                                           StylePassScheduler scheduler,
                                           RenderTreeState::ComputeFrameStyles compute) {
  auto tentative = warnings.remainingAllowance();
  const auto rejected = [](const Registry& current) {
    return current.ctx().get<StyleResourceBudget>().rejected();
  };
  if (!compute(registry, tentative, scheduler, rejected)) {
    return false;
  }
  warnings.merge(std::move(tentative));
  return true;
}

namespace {

bool CanScanStylesheetEdit(const Registry& registry, const RenderTreeState* state,
                           std::size_t removedCount, std::size_t addedCount) {
  constexpr std::size_t kMaximumRulesPerEdit = 32;
  return state && state->hasBeenBuilt && !state->needsFullRebuild &&
         !state->needsFullStyleRecompute && !RequiresFullStyleBudgetReplay(registry) &&
         removedCount <= kMaximumRulesPerEdit && addedCount <= kMaximumRulesPerEdit &&
         registry.view<const donner::components::TreeComponent>().size() <=
             StyleSystem::PreparedBatch::kMaximumElements;
}

bool NeedsRuleEditProbe(const Registry& registry, Entity entity) {
  if (registry.all_of<ComputedStyleComponent>(entity)) {
    return true;
  }
  // A prior live edit can remove the computed record before the next frame.
  const auto* dirty = registry.try_get<DirtyFlagsComponent>(entity);
  return dirty && dirty->test(DirtyFlagsComponent::Style);
}

bool MatchChangedRules(const Registry& registry, std::span<const css::SelectorRule> rules,
                       StyleResourceBudget& budget, std::vector<Entity>& matches) {
  if (rules.empty()) {
    return true;
  }
  const RuleIndex index(rules);
  std::vector<std::uint32_t> candidates;
  for (const Entity entity : registry.view<const donner::components::TreeComponent>()) {
    if (!NeedsRuleEditProbe(registry, entity)) {
      continue;
    }
    const Entity source = StyleDataEntity(registry, entity);
    const RuleIndexElementView keys(registry, entity, source);
    CollectCandidateRules(rules, index, keys, false, candidates);
    const ShadowedElementAdapter adapter(registry, entity, source);
    for (const auto candidate : candidates) {
      if (!budget.reserveRuleElementMatch()) {
        return false;
      }
      if (MatchSelectorRule(rules[candidate], adapter, &budget.selectorTraversal())) {
        matches.push_back(entity);
        break;
      }
      if (budget.rejected()) {
        return false;
      }
    }
  }
  return true;
}

bool CrossesStyleEditBoundary(const Registry& registry, Entity entity) {
  using Tree = donner::components::TreeComponent;
  for (Entity current = entity; current != entt::null;
       current = registry.get<Tree>(current).parent()) {
    const Entity source = StyleDataEntity(registry, current);
    if (StyleSystem::isStyleEditSubtreeBoundary(registry, source) ||
        registry.all_of<ExternalUseComponent>(source)) {
      return true;
    }
  }
  return false;
}

bool ExpandChangedStyleSubtrees(Registry& registry, std::vector<Entity>& affected) {
  using Tree = donner::components::TreeComponent;
  std::unordered_set<Entity> seen;
  std::erase_if(affected, [&](Entity entity) { return !seen.insert(entity).second; });
  if (std::ranges::any_of(
          affected, [&](Entity entity) { return CrossesStyleEditBoundary(registry, entity); })) {
    return false;
  }
  for (std::size_t i = 0; i < affected.size(); ++i) {
    const Entity entity = affected[i];
    const Entity source = StyleDataEntity(registry, entity);
    if (StyleSystem::isStyleEditSubtreeBoundary(registry, source)) {
      return false;
    }
    for (Entity child = registry.get<Tree>(entity).firstChild(); child != entt::null;
         child = registry.get<Tree>(child).nextSibling()) {
      if (seen.insert(child).second) {
        affected.push_back(child);
      }
    }
  }
  return ShadowTreeSystem::appendStyleOccurrences(registry, affected);
}

}  // namespace

void StyleSystem::markEditedStyleTargets(Registry& registry, std::span<const Entity> affected,
                                         ApplyRetainedStyle applyRetained) {
  if (affected.empty()) {
    return;
  }
  auto& state = registry.ctx().get<RenderTreeState>();
  std::vector<uint32_t> oldOrder;
  oldOrder.reserve(affected.size());
  for (Entity entity : affected) {
    const auto order = StyleSystem::retainedStyleOrder(registry, entity);
    if (!order) {
      break;
    }
    oldOrder.push_back(*order);
  }
  const bool retainable = applyRetained && oldOrder.size() == affected.size();
  if (retainable) {
    state.applyLiveStyleUpdate = applyRetained;
  }
  constexpr uint16_t kNarrowed = DirtyFlagsComponent::Style | DirtyFlagsComponent::RenderInstance |
                                 DirtyFlagsComponent::NarrowedStyle;
  const uint16_t flags = retainable ? kNarrowed : DirtyFlagsComponent::StyleCascade;
  for (std::size_t i = 0; i < affected.size(); ++i) {
    const EntityHandle node(registry, affected[i]);
    StyleSystem().invalidateComputed(node);
    node.remove<ComputedStopComponent>();
    auto& dirty = node.get_or_emplace<DirtyFlagsComponent>();
    // A computed-style read between edits can materialize the intermediate style.
    // Compare against the last prepared frame's order, recorded by the first edit.
    if (retainable && !dirty.test(DirtyFlagsComponent::NarrowedStyle)) {
      dirty.narrowedStyleOrder = oldOrder[i];
    }
    dirty.mark(flags);
  }
  state.computeEditedFrameStyles = StyleSystem::computeEditedFrameStyles;
}

bool StyleSystem::invalidateStylesheetRules(Registry& registry,
                                            std::span<const css::SelectorRule> removed,
                                            std::span<const css::SelectorRule> added,
                                            ApplyRetainedStyle applyRetained) {
  auto* state = registry.ctx().find<RenderTreeState>();
  if (!CanScanStylesheetEdit(registry, state, removed.size(), added.size())) {
    return false;
  }
  const auto* existing = registry.ctx().find<StyleResourceBudget>();
  StyleResourceBudget scan(existing ? existing->limits() : StyleResourceBudget::Limits());
  std::vector<Entity> affected;
  if (!MatchChangedRules(registry, removed, scan, affected) ||
      !MatchChangedRules(registry, added, scan, affected) ||
      !ExpandChangedStyleSubtrees(registry, affected)) {
    return false;
  }
  if (auto* cache = registry.ctx().find<MatchedPropertiesCache>()) {
    cache->clear();
  }
  markEditedStyleTargets(registry, affected, applyRetained);
  return true;
}

std::uint8_t StyleSystem::styleEditDependencies(const Registry& registry,
                                                std::string_view attribute,
                                                std::span<const std::string_view> changedClasses) {
  if (RequiresFullStyleBudgetReplay(registry)) {
    // Prior refusals can leave fallback values. A custom budget can also change admission order
    // during a narrow pass even when the previous full pass succeeded.
    return SelectorDependency::Ancestors;
  }
  std::uint8_t relations = 0;
  for (auto view = registry.view<const StylesheetComponent>(); auto entity : view) {
    const auto& sheet = view.get<const StylesheetComponent>(entity);
    if (!HasSvgNamespace(registry, entity) || !sheet.isCssType()) {
      continue;
    }
    const auto& summary = sheet.dependencySummary;
    if (summary.usesHas() || summary.usesAnyAttributeName()) {
      return SelectorDependency::Ancestors;
    }
    if (!attribute.empty()) {
      relations |= summary.attributeDependency(attribute);
    }
    for (std::string_view name : changedClasses) {
      relations |= summary.classDependency(name);
    }
  }
  return relations;
}

bool StyleSystem::isStyleEditSubtreeBoundary(const Registry& registry, Entity entity) {
  static constexpr auto boundaries = [] {
    std::array<bool, kElementTypeCount> result{};
    for (ElementType type :
         {ElementType::ClipPath, ElementType::Filter, ElementType::LinearGradient,
          ElementType::Marker, ElementType::Mask, ElementType::Pattern, ElementType::RadialGradient,
          ElementType::Stop, ElementType::Text, ElementType::TextPath, ElementType::TSpan}) {
      result[static_cast<std::size_t>(type)] = true;
    }
    return result;
  }();
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  return type && type->inSvgNamespace() && boundaries[static_cast<std::size_t>(type->type())];
}

namespace {

bool StyleEditTouchesNonlocalSource(const Registry& registry, Entity entity) {
  using Tree = donner::components::TreeComponent;
  for (Entity current = entity; current != entt::null;
       current = registry.get<Tree>(current).parent()) {
    const Entity source = StyleDataEntity(registry, current);
    const auto* type = registry.try_get<ElementTypeComponent>(source);
    if (StyleSystem::isStyleEditSubtreeBoundary(registry, source) ||
        (type && type->type() == ElementType::Symbol) ||
        registry.all_of<ExternalUseComponent>(source)) {
      return true;
    }
  }
  return false;
}

bool HasLocalDeferredStyleGeometry(const PropertyRegistry& style, ElementType type) {
  if (type == ElementType::SVG || (type == ElementType::Use && !style.unparsedProperties.empty())) {
    return false;
  }
  constexpr std::string_view kGeometryNames[] = {"d", "width", "height", "x",  "y",  "cx", "cy",
                                                 "r", "rx",    "ry",     "x1", "x2", "y1", "y2"};
  return std::ranges::all_of(style.unparsedProperties, [&](const auto& entry) {
    return std::ranges::find(kGeometryNames, std::string_view(entry.first)) !=
           std::end(kGeometryNames);
  });
}

bool HasLocalStyleTransform(const PropertyRegistry& style) {
  const auto origin = style.transformOrigin.get();
  // The SVG user-agent sheet explicitly sets 0 0. Units do not change a zero-length origin.
  return origin && origin->x.value == 0 && origin->y.value == 0 &&
         style.vectorEffect.get() == VectorEffect::None && style.isolation.get() == Isolation::Auto;
}

bool HasLocalStylePaint(const PropertyRegistry& style) {
  const auto localPaint = [](const auto& paint) {
    return !paint || paint->template is<PaintServer::None>() ||
           paint->template is<PaintServer::Solid>();
  };
  return localPaint(style.fill.get()) && localPaint(style.stroke.get());
}

bool HasStyleResourceEffects(const PropertyRegistry& style) {
  return style.clipPath.get() || style.mask.get() || style.markerStart.get() ||
         style.markerMid.get() || style.markerEnd.get() ||
         (style.filter.value && !style.filter.value->empty());
}

}  // namespace

std::optional<uint32_t> StyleSystem::retainedStyleOrder(const Registry& registry, Entity entity) {
  const auto* computed = registry.try_get<ComputedStyleComponent>(entity);
  if (!computed || !computed->style || computed->externalFill || computed->externalStroke ||
      StyleEditTouchesNonlocalSource(registry, entity)) {
    return std::nullopt;
  }

  const auto& style = *computed->properties();
  const auto* type = registry.try_get<ElementTypeComponent>(StyleDataEntity(registry, entity));
  if (!HasLocalDeferredStyleGeometry(style, type ? type->type() : ElementType::Unknown) ||
      !HasLocalStyleTransform(style) || !HasLocalStylePaint(style) ||
      HasStyleResourceEffects(style)) {
    return std::nullopt;
  }

  // Compare order and visibility inputs without retaining the old computed style.
  return static_cast<uint32_t>(*style.display.get()) |
         (static_cast<uint32_t>(*style.visibility.get()) << 8) |
         (static_cast<uint32_t>(*style.overflow.get()) << 16) |
         (static_cast<uint32_t>(*style.mixBlendMode.get()) << 24);
}

void StyleSystem::invalidateComputed(EntityHandle handle) {
  handle.remove<ComputedStyleComponent>();
}

void StyleSystem::invalidateAll(EntityHandle handle) {
  invalidateComputed(handle);
  // TODO: Reparse attributes.
}

}  // namespace donner::svg::components
