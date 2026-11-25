#include "donner/svg/components/style/StyleSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>

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
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/PaintAnimationStyle.h"
#include "donner/svg/components/style/RuleIndex.h"
#include "donner/svg/components/style/ScalarAnimationStyle.h"
#include "donner/svg/components/style/SelectorContent.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"
#include "donner/svg/components/style/StyleComponent.h"

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

/// Register the `@font-face` rules of every stylesheet with the document's resources.
void SynchronizeAllStylesheetFonts(Registry& registry) {
  ResourceManagerContext& resourceManager = registry.ctx().get<ResourceManagerContext>();
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    auto [stylesheet] = view.get(stylesheetEntity);
    SynchronizeStylesheetFonts(resourceManager, registry, stylesheetEntity, stylesheet);
  }
}

bool IsActiveAuthorStylesheet(const Registry& registry, Entity entity,
                              const StylesheetComponent& stylesheet) {
  return HasSvgNamespace(registry, entity) && !stylesheet.isUserAgentStylesheet &&
         !stylesheet.sourceMap.empty();
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
  explicit RegistryCopyCount(StyleCascadeMetrics& metrics)
      : metrics_(metrics),
        outermost_(metrics.activeCascades++ == 0),
        start_(outermost_ ? details::PropertyRegistryCopyCount() : 0) {}
  ~RegistryCopyCount() {
    --metrics_.activeCascades;
    if (outermost_) {
      metrics_.counters.registryCopies += details::PropertyRegistryCopyCount() - start_;
    }
  }
  RegistryCopyCount(const RegistryCopyCount&) = delete;
  RegistryCopyCount& operator=(const RegistryCopyCount&) = delete;

private:
  StyleCascadeMetrics& metrics_;
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
    if (isShadowTreeRoot(treeEntity_)) {
      return std::nullopt;
    }
    const Entity target =
        registry_.get().get<donner::components::TreeComponent>(treeEntity_).previousSibling();
    return findElementSibling(target, false, budget);
  }

  std::optional<ShadowedElementAdapter> nextSibling(
      css::SelectorTraversalBudget* budget = nullptr) const {
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

/// An element as the rule index reads it: straight from its components, without copies, and only
/// for the key kinds an index has. Valid for one cascade of the element. Like
/// \ref ShadowedElementAdapter, a shadow entity reads its tag from its own tree entity and its id,
/// classes and attributes from its light entity.
class RuleIndexElementView {
public:
  RuleIndexElementView(const Registry& registry, Entity treeEntity, Entity dataEntity)
      : registry_(registry), treeEntity_(treeEntity), dataEntity_(dataEntity) {}

  std::string_view id() const {
    const auto* component = registry_.try_get<IdComponent>(dataEntity_);
    return component != nullptr ? std::string_view(component->id()) : std::string_view();
  }

  std::string_view typeName() const {
    return registry_.get<donner::components::TreeComponent>(treeEntity_).tagName().name;
  }

  template <typename Visitor>
  void forEachClassName(Visitor&& visit) const {
    if (const auto* component = registry_.try_get<ClassComponent>(dataEntity_)) {
      for (std::string_view name : StringUtils::SplitAsciiWhitespace(component->className)) {
        visit(name);
      }
    }
  }

  template <typename Visitor>
  void forEachAttributeLocalName(Visitor&& visit) const {
    if (const auto* component =
            registry_.try_get<donner::components::AttributesComponent>(dataEntity_)) {
      component->forEachAttributeName(
          [&](const xml::XMLQualifiedNameRef& name) { visit(std::string_view(name.name)); });
    }
  }

private:
  const Registry& registry_;
  Entity treeEntity_;
  Entity dataEntity_;
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

const ComputedStyleComponent* FindRootStyle(Registry& registry,
                                            const SVGDocumentContext* document) {
  if (document && registry.valid(document->rootEntity)) {
    return registry.try_get<ComputedStyleComponent>(document->rootEntity);
  }
  return nullptr;
}

const ComputedStyleComponent* FindParentStyle(Registry& registry,
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

const ComputedStyleComponent* EnsureParentStyle(StyleSystem& styleSystem, Registry& registry,
                                                const ExternalUseStyleContext* occurrence,
                                                Entity parent,
                                                const ComputedStyleComponent* parentStyle,
                                                ParseWarningSink& warningSink) {
  if (!occurrence && parent != entt::null && (!parentStyle || !parentStyle->style)) {
    return &styleSystem.computeStyle(EntityHandle(registry, parent), warningSink);
  }
  return parentStyle;
}

void PopulateMatchedPropertiesKey(Registry& registry, EntityHandle handle, Entity dataEntity,
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

bool FindAndInstallMatchedStyle(Registry& registry, Entity parent, MatchedPropertiesKey& key,
                                std::size_t matchedDeclarationParsesSkipped,
                                StyleResourceBudget& styleBudget, StyleCascadeCounters& counters,
                                ComputedStyleComponent& computed, MatchedCacheState& state) {
  SetMatchedKeyRecords(key, state);
  const MatchedPropertiesEntry* entry = GetMatchedPropertiesCache(registry).find(key);
  if (!entry || !ChargeCachedResourceOpacityWork(entry->style->properties, styleBudget, counters)) {
    return false;
  }
  const bool allowPaint =
      parent == entt::null || !registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
  computed.externalFill =
      ExternalPaintOrigin(entry->fillState, false, state.parentStyle, false, allowPaint);
  computed.externalStroke =
      ExternalPaintOrigin(entry->strokeState, false, state.parentStyle, true, allowPaint);
  computed.style = entry->style;
  ++counters.matchedPropertiesCacheHits;
  counters.declarationParsesSkipped += matchedDeclarationParsesSkipped;
  return true;
}

MatchedCacheState PrepareMatchedCacheState(Registry& registry, Entity dataEntity, Entity parent,
                                           const ExternalUseStyleContext* occurrence,
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

bool TryInstallMatchedStyle(Registry& registry, Entity parent, MatchedPropertiesKey& key,
                            std::size_t deferredDeclarations, StyleResourceBudget& styleBudget,
                            StyleCascadeCounters& counters, ComputedStyleComponent& computed,
                            MatchedCacheState& state) {
  if (!state.initiallyCacheable || !state.parentReady || !state.rootReady) {
    if (state.initiallyCacheable) {
      ++counters.matchedPropertiesCacheMisses;
      state.countedCacheMiss = true;
    } else if (state.cacheEnabled) {
      ++counters.matchedPropertiesCacheUncacheable;
    }
    return false;
  }
  if (!FindAndInstallMatchedStyle(registry, parent, key, deferredDeclarations, styleBudget,
                                  counters, computed, state)) {
    ++counters.matchedPropertiesCacheMisses;
    state.countedCacheMiss = true;
    return false;
  }
  return true;
}

bool TryInstallLateMatchedStyle(Registry& registry, Entity parent, MatchedPropertiesKey& key,
                                bool warningBeforeParent, StyleResourceBudget& styleBudget,
                                StyleCascadeCounters& counters, ComputedStyleComponent& computed,
                                MatchedCacheState& state) {
  if (!state.initiallyCacheable || warningBeforeParent || styleBudget.rejected()) {
    return false;
  }
  state.rootStyle = FindRootStyle(registry, state.document);
  state.rootReady = RootStyleReady(key, state.document, registry, state.rootStyle);
  if (!state.rootReady || !FindAndInstallMatchedStyle(registry, parent, key, 0, styleBudget,
                                                      counters, computed, state)) {
    return false;
  }
  if (state.countedCacheMiss) {
    --counters.matchedPropertiesCacheMisses;
    state.countedCacheMiss = false;
  }
  return true;
}

void FinishMatchedCacheMiss(Registry& registry, MatchedPropertiesKey& key, PropertyState fillState,
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
    return;
  }
  state.rootStyle = FindRootStyle(registry, state.document);
  SetMatchedKeyRecords(key, state);
  GetMatchedPropertiesCache(registry).insert(
      key, MatchedPropertiesEntry{computed.style, fillState, strokeState});
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

  void admit(const css::Declaration& declaration, css::Specificity specificity,
             const StyleComponent* local, ParseWarningSink& sink) {
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
};

void StyleSystem::applyStylesheetRules(Registry& registry, Entity treeEntity, Entity dataEntity,
                                       StyleResourceBudget& styleBudget,
                                       StyleCascadeCounters& counters,
                                       MatchedPropertiesKey& cacheKey, const StyleComponent* local,
                                       ParseWarningSink& warningSink,
                                       PendingStyleDeclarations& declarations) {
  const ShadowedElementAdapter adapter(registry, treeEntity, dataEntity);
  const RuleIndexElementView elementKeys(registry, treeEntity, dataEntity);
  const bool unindexed = registry.ctx().contains<UnindexedCascadeForTesting>();
  CandidateListScope candidateScope;
  std::vector<std::uint32_t>& candidates = candidateScope.list();
  // Apply one sheet's candidate rules in rule order; returns false once the style budget is
  // exhausted. Rules that cannot match are not candidates and cost nothing.
  const auto applyRules = [&](std::uint64_t sheetRevision, std::size_t sheetOrdinal,
                              const css::Stylesheet& sheet, const RuleIndex& index,
                              bool isUserAgentStylesheet) {
    const std::span<const css::SelectorRule> rules = sheet.rules();
    CollectCandidateRules(rules, index, elementKeys, unindexed, candidates);
    for (const std::uint32_t position : candidates) {
      const css::SelectorRule& rule = rules[position];
      if (!styleBudget.reserveRuleElementMatch()) {
        return false;
      }
      ++counters.rulesTested;
      auto match = MatchSelectorRule(rule, adapter, &styleBudget.selectorTraversal());
      if (!match) {
        continue;
      }
      ++counters.rulesMatched;
      css::Specificity specificity = match->specificity;
      if (isUserAgentStylesheet) {
        specificity = specificity.toUserAgentSpecificity();
      }
      cacheKey.addRule(sheetRevision, sheetOrdinal, position, specificity);
      AdmitRuleDeclarations(
          rule, specificity, styleBudget, counters,
          [&](const css::Declaration& declaration, css::Specificity declarationSpecificity) {
            declarations.admit(declaration, declarationSpecificity, local, warningSink);
          });
    }
    return true;
  };

  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    const auto& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
    if (!IsApplicableStylesheet(registry, stylesheet, stylesheetEntity, dataEntity)) {
      continue;
    }
    // Imported rules precede the importing sheet's own rules in the cascade.
    UTILS_RELEASE_ASSERT(stylesheet.importedRuleIndexes.size() ==
                         stylesheet.importedStylesheets.size());
    for (std::size_t i = 0; i < stylesheet.importedStylesheets.size(); ++i) {
      if (!applyRules(stylesheet.contentRevision(), i, stylesheet.importedStylesheets[i],
                      stylesheet.importedRuleIndexes[i], stylesheet.isUserAgentStylesheet)) {
        return;
      }
    }
    if (!applyRules(stylesheet.contentRevision(), stylesheet.importedStylesheets.size(),
                    stylesheet.stylesheet, stylesheet.ruleIndex,
                    stylesheet.isUserAgentStylesheet)) {
      return;
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
  return stroke ? style.properties()->stroke.get() : style.properties()->fill.get();
}

void StyleSystem::inheritComputedParent(EntityHandle handle, Entity parent,
                                        PropertyRegistry& properties,
                                        ComputedStyleComponent& computed,
                                        ParseWarningSink& warningSink) {
  Registry& registry = *handle.registry();
  const auto* occurrence = ExternalRootContext(registry, handle.entity());
  const PropertyState fillState = properties.fill.state;
  const PropertyState strokeState = properties.stroke.state;
  if (occurrence) {
    const LocalResourceStates local(properties);
    inheritExternalParent(properties, *occurrence->parent.properties());
    if (BlockExternalResources(properties, local)) {
      rejectExternalHostResource(registry);
      warningSink.add(ParseDiagnostic::Error("External-use inherited URL resources are unsupported",
                                             FileOffset::Offset(0)));
    }
  } else {
    inheritProperties(registry, parent, properties, warningSink);
  }
  const auto* parentStyle =
      parent != entt::null ? registry.try_get<ComputedStyleComponent>(parent) : nullptr;
  const bool allowPaint = occurrence || !registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
  computed.externalFill =
      ExternalPaintOrigin(fillState, occurrence != nullptr, parentStyle, false, allowPaint);
  computed.externalStroke =
      ExternalPaintOrigin(strokeState, occurrence != nullptr, parentStyle, true, allowPaint);
}

void StyleSystem::resolveComputedParentFonts(EntityHandle handle, Entity parent,
                                             PropertyRegistry& properties) {
  if (const auto* occurrence = ExternalRootContext(*handle.registry(), handle.entity())) {
    const PropertyRegistry& host = *occurrence->parent.properties();
    properties.resolveFontSize(host.fontSize.get().value().value);
    properties.resolveFontWeight(host.fontWeight.get().value());
    properties.resolveFontStretch(host.fontStretch.get().value());
  } else {
    resolveRelativeFontProperties(*handle.registry(), parent, properties);
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

void StyleSystem::inheritProperties(Registry& registry, Entity parent, PropertyRegistry& properties,
                                    ParseWarningSink& warningSink) {
  if (parent == entt::null) {
    return;
  }
  auto& parentStyleComponent = registry.get_or_emplace<ComputedStyleComponent>(parent);
  computePropertiesInto(EntityHandle(registry, parent), parentStyleComponent, warningSink);
  const PropertyInheritOptions inheritOptions = registry.all_of<DoNotInheritFillOrStrokeTag>(parent)
                                                    ? PropertyInheritOptions::NoPaint
                                                    : PropertyInheritOptions::All;
  InheritPreservingLocalValues(properties, *parentStyleComponent.properties(), inheritOptions);
}

void StyleSystem::resolveRelativeFontProperties(Registry& registry, Entity parent,
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

void StyleSystem::computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                                        ParseWarningSink& warningSink) {
  if (computedStyle.style) {
    return;  // Already computed.
  }

  Registry& registry = *handle.registry();
  auto& styleBudget = GetStyleResourceBudget(registry);
  StyleCascadeMetrics& metrics = GetStyleCascadeMetrics(registry);
  const RegistryCopyCount copyCount(metrics);
  StyleCascadeCounters& counters = metrics.counters;
  ++counters.elementsCascaded;

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();
  const Entity parent = handle.get<donner::components::TreeComponent>().parent();
  const auto* occurrence = ExternalRootContext(registry, handle.entity());
  const auto* styleComponent = registry.try_get<StyleComponent>(dataEntity);
  const std::size_t warningAttemptsBefore = warningSink.warningAttempts();

  MatchedPropertiesKey cacheKey;
  PendingStyleDeclarations declarations;
  applyStylesheetRules(registry, handle.entity(), dataEntity, styleBudget, counters, cacheKey,
                       styleComponent, warningSink, declarations);

  PopulateMatchedPropertiesKey(registry, handle, dataEntity, parent, styleComponent, cacheKey);
  MatchedCacheState cacheState =
      PrepareMatchedCacheState(registry, dataEntity, parent, occurrence,
                               declarations.streamed != nullptr, cacheKey, styleBudget);
  if (TryInstallMatchedStyle(registry, parent, cacheKey, declarations.entries.size(), styleBudget,
                             counters, computedStyle, cacheState)) {
    return;
  }

  // Apply local style. The record is built in place and stored once it is final, after which it
  // is never modified.
  auto record = declarations.materialize(styleComponent, warningSink);
  PropertyRegistry& properties = record->properties;
  ApplyAnimatedStyle(registry, dataEntity, properties);
  const bool warningBeforeParent = warningSink.warningAttempts() != warningAttemptsBefore;
  cacheState.parentStyle =
      EnsureParentStyle(*this, registry, occurrence, parent, cacheState.parentStyle, warningSink);
  const std::size_t warningAttemptsAfterParent = warningSink.warningAttempts();
  if (TryInstallLateMatchedStyle(registry, parent, cacheKey, warningBeforeParent, styleBudget,
                                 counters, computedStyle, cacheState)) {
    return;
  }
  ApplyContextualFontSize(registry, dataEntity, cacheState.parentStyle, properties);
  const auto resourceOpacities = ApplyScalarAnimationStyles(
      registry, dataEntity, cacheState.parentStyle, properties, styleBudget, warningSink);
  ApplySymbolInstanceDisplay(handle, dataEntity, properties);
  properties.resolveElementColors(warningSink);
  const bool inheritPaint =
      occurrence || parent == entt::null || !registry.all_of<DoNotInheritFillOrStrokeTag>(parent);
  ApplyPaintAnimationStyles(registry, dataEntity, cacheState.parentStyle, inheritPaint, properties);
  const PropertyState fillState = properties.fill.state;
  const PropertyState strokeState = properties.stroke.state;

  // Inherit from parent, then charge the dynamic bytes retained by the final computed style.
  // Charging the parent's source value alone misses large locally parsed vectors and can also
  // differ from the actual value selected by the cascade.
  inheritComputedParent(handle, parent, properties, computedStyle, warningSink);

  const std::size_t retainedBytes = properties.complexPropertyBytes();
  if (!styleBudget.canRetainComputedStyle(retainedBytes)) {
    if (auto* existingCache = registry.ctx().find<MatchedPropertiesCache>()) {
      existingCache->clear();
    }
  }
  if (styleBudget.retainComputedStyle(*record, retainedBytes)) {
    record->resourceOpacities = resourceOpacities;
  } else {
    // Once the aggregate retained-byte budget is exhausted, use bounded initial styles for the
    // remainder of the pass instead of retaining attacker-sized local or inherited vectors.
    computedStyle.externalFill = false;
    computedStyle.externalStroke = false;
    properties = PropertyRegistry();
    record->resourceOpacities = ComputedResourceOpacities();
  }

  // Resolve font-size to absolute pixels. CSS spec requires the computed value of font-size to
  // always be an absolute length. Relative units (em, %, ex) resolve against the parent's computed
  // font-size, and percentages resolve against the parent font-size (not the viewBox). Local
  // text-indent em lengths compute from that result before descendants inherit them.
  resolveComputedParentFonts(handle, parent, properties);
  const double rootFontSizePx = ComputedRootFontSize(registry, handle.entity(), properties);
  properties.resolveTextDecorationThickness(rootFontSizePx);
  properties.resolveInsetClipLengths(rootFontSizePx);
  computedStyle.style = std::move(record);

  FinishMatchedCacheMiss(registry, cacheKey, fillState, strokeState, warningBeforeParent,
                         warningAttemptsAfterParent, warningSink, styleBudget, counters,
                         computedStyle, cacheState);
}

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
  for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
    const auto& stylesheet = view.get<StylesheetComponent>(stylesheetEntity);
    if (!IsApplicableStylesheet(registry, stylesheet, stylesheetEntity, dataEntity)) {
      continue;
    }
    const std::span<const css::SelectorRule> rules = stylesheet.stylesheet.rules();
    CollectCandidateRules(rules, stylesheet.ruleIndex, elementKeys, unindexed, candidates);

    for (const std::uint32_t ruleIndex : candidates) {
      if (!styleBudget.reserveRuleElementMatch()) {
        break;
      }
      const css::SelectorRule& rule = rules[ruleIndex];
      std::optional<MatchedSelectorEntry> match =
          MatchSelectorRule(rule, adapter, &styleBudget.selectorTraversal());
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

void StyleSystem::computeAllStyles(Registry& registry, ParseWarningSink& warningSink,
                                   FullRestyle fullRestyle) {
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
  // Mutations whose effect is not local to specific entities (stylesheet edits, class/id/attribute
  // writes that feed selectors, structural changes) cannot express themselves through per-entity
  // flags at all, and must instead request RenderTreeState::needsFullStyleRecompute.
  if (hasBeenBuilt && hasDirtyEntities && !needsFullStyleRecompute) {
    computeDirtyStyles(registry, warningSink);
  } else {
    if (hasBeenBuilt && needsFullStyleRecompute &&
        fullRestyle == FullRestyle::DiscardAndRecompute) {
      discardComputedStylesForFullRestyle(registry);
    }
    computeMissingStyles(registry, warningSink);
  }

  SynchronizeAllStylesheetFonts(registry);
}

void StyleSystem::computeDirtyStyles(Registry& registry, ParseWarningSink& warningSink) {
  for (auto entity : registry.view<DirtyFlagsComponent>()) {
    if (!registry.get<DirtyFlagsComponent>(entity).test(DirtyFlagsComponent::Style)) {
      continue;
    }

    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
    computeStyle(EntityHandle(registry, entity), warningSink);
  }
}

void StyleSystem::computeMissingStyles(Registry& registry, ParseWarningSink& warningSink) {
  // Create placeholder ComputedStyleComponents for all elements in the range, since creating
  // computed style components also creates the parents, and we can't modify the component list
  // while iterating it.
  auto view = registry.view<donner::components::TreeComponent>();
  for (auto entity : view) {
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }

  // Compute the styles for all elements; computeStyle returns early for computed ones.
  for (auto entity : view) {
    computeStyle(EntityHandle(registry, entity), warningSink);
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
  const StyleTimeScope timeScope(GetStyleCascadeMetrics(registry));
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
  handle.remove<ComputedStyleComponent>();
}

void StyleSystem::invalidateAll(EntityHandle handle) {
  invalidateComputed(handle);
  // TODO: Reparse attributes.
}

}  // namespace donner::svg::components
