#include "donner/svg/components/filter/FilterSystem.h"

#include <array>
#include <limits>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/ReferenceResolutionBudget.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/filter/ComputedFilterResourceBudget.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/core/ImageRendering.h"
#include "donner/svg/graph/RecursionGuard.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

namespace {

struct ComputedFilterBudgetListenerInstalled {};

void OnComputedFilterDestroyed(Registry& registry, Entity entity) {
  if (auto* budget = registry.ctx().find<ComputedFilterResourceBudget>()) {
    budget->release(entity);
  }
}

ReferenceResolutionBudget& GetReferenceResolutionBudget(Registry& registry) {
  if (!registry.ctx().contains<ReferenceResolutionBudget>()) {
    registry.ctx().emplace<ReferenceResolutionBudget>();
  }
  return registry.ctx().get<ReferenceResolutionBudget>();
}

ComputedFilterResourceBudget& GetComputedFilterResourceBudget(Registry& registry) {
  if (!registry.ctx().contains<ComputedFilterResourceBudget>()) {
    std::shared_ptr<DocumentResourceFamilyBudget> family;
    if (const auto* context = registry.ctx().find<DocumentResourceFamilyContext>()) {
      family = context->budget;
    }
    registry.ctx().emplace<ComputedFilterResourceBudget>(std::move(family));
  }
  if (!registry.ctx().contains<ComputedFilterBudgetListenerInstalled>()) {
    registry.ctx().emplace<ComputedFilterBudgetListenerInstalled>();
    static_cast<void>(registry.storage<ComputedFilterComponent>());
    registry.on_destroy<ComputedFilterComponent>().connect<&OnComputedFilterDestroyed>();
  }
  return registry.ctx().get<ComputedFilterResourceBudget>();
}

bool AddRetainedBytes(std::size_t& total, std::size_t additional) {
  if (additional > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += additional;
  return true;
}

bool AddRetainedProduct(std::size_t& total, std::size_t count, std::size_t elementSize) {
  if (elementSize != 0 && count > std::numeric_limits<std::size_t>::max() / elementSize) {
    return false;
  }
  return AddRetainedBytes(total, count * elementSize);
}

bool AddAnimatedRegionRetainedBytes(std::size_t& bytes, const FilterNode& node) {
  for (const auto& recipe : node.animatedRegion) {
    if (recipe && !AddRetainedBytes(bytes, recipe->retainedBytes)) {
      return false;
    }
  }
  return true;
}

bool AddFilterInputRetainedBytes(std::size_t& bytes, const FilterNode& node) {
  if (!AddRetainedProduct(bytes, node.inputs.capacity(), sizeof(FilterInput)) ||
      !AddRetainedBytes(bytes,
                        static_cast<std::size_t>(FilterPrimitivePayloadBytes(node.primitive)))) {
    return false;
  }
  for (const FilterInput& input : node.inputs) {
    const auto* named = std::get_if<FilterInput::Named>(&input.value);
    if (named && !AddRetainedBytes(bytes, named->name.size())) {
      return false;
    }
  }
  return true;
}

bool AddFilterImageRetainedBytes(std::size_t& bytes, const FilterNode& node) {
  const auto* image = std::get_if<filter_primitive::Image>(&node.primitive);
  if (!image) {
    return true;
  }
  return AddRetainedBytes(bytes, image->href.size()) &&
         AddRetainedBytes(bytes, image->fragmentId.size());
}

bool AddFilterNodeRetainedBytes(std::size_t& bytes, const FilterNode& node) {
  if (!AddAnimatedRegionRetainedBytes(bytes, node) || !AddFilterInputRetainedBytes(bytes, node)) {
    return false;
  }
  if (node.result && !AddRetainedBytes(bytes, node.result->size())) {
    return false;
  }
  return AddFilterImageRetainedBytes(bytes, node);
}

std::optional<std::size_t> ComputedFilterRetainedBytes(
    const FilterGraph& graph, const std::vector<FilterEffect>& effectChain) {
  std::size_t bytes = sizeof(ComputedFilterComponent);
  if (!AddRetainedProduct(bytes, effectChain.capacity(), sizeof(FilterEffect)) ||
      !AddRetainedProduct(bytes, graph.nodes.capacity(), sizeof(FilterNode))) {
    return std::nullopt;
  }
  for (const FilterNode& node : graph.nodes) {
    if (!AddFilterNodeRetainedBytes(bytes, node)) {
      return std::nullopt;
    }
  }
  return bytes;
}

void RemoveComputedFilter(EntityHandle handle) {
  GetComputedFilterResourceBudget(*handle.registry());
  handle.remove<ComputedFilterComponent>();
}

/// Parse the `in` attribute from a FilterPrimitiveComponent into a FilterInput.
FilterInput toFilterInput(const FilterPrimitiveComponent& primitive) {
  if (!primitive.in.has_value()) {
    return FilterInput{FilterInput::Previous{}};
  }
  return primitive.in.value();
}

/// Convert optional `in2` to a FilterInput (defaults to Previous).
FilterInput toFilterInput2(const FilterPrimitiveComponent& primitive) {
  if (!primitive.in2.has_value()) {
    return FilterInput{FilterInput::Previous{}};
  }
  return primitive.in2.value();
}

/// Populate the common FilterNode fields from the primitive's standard attributes.
/// For single-input primitives.
FilterNode makeFilterNode(FilterPrimitive primitive, const FilterPrimitiveComponent& attrs,
                          std::optional<ColorInterpolationFilters> cif = std::nullopt) {
  FilterNode node;
  node.primitive = std::move(primitive);
  node.inputs.push_back(toFilterInput(attrs));
  node.result = attrs.result;
  node.x = attrs.x;
  node.y = attrs.y;
  node.width = attrs.width;
  node.height = attrs.height;
  node.colorInterpolationFilters = cif;
  return node;
}

/// Populate the common FilterNode fields for two-input primitives (in + in2).
FilterNode makeFilterNode2(FilterPrimitive primitive, const FilterPrimitiveComponent& attrs,
                           std::optional<ColorInterpolationFilters> cif = std::nullopt) {
  FilterNode node;
  node.primitive = std::move(primitive);
  node.inputs.push_back(toFilterInput(attrs));
  node.inputs.push_back(toFilterInput2(attrs));
  node.result = attrs.result;
  node.x = attrs.x;
  node.y = attrs.y;
  node.width = attrs.width;
  node.height = attrs.height;
  node.colorInterpolationFilters = cif;
  return node;
}

/// Apply canonical flood opacity, or parse the declaration of a manually constructed style.
/// @param opacity Destination property. @param style Source style. @param warnings Parse
/// diagnostics.
void ApplyFloodOpacity(Property<double>& opacity, const ComputedStyleComponent& style,
                       ParseWarningSink& warnings) {
  if (style.resourceOpacities) {
    opacity.set(style.resourceOpacities->floodOpacity, style.resourceOpacities->floodSpecificity);
    return;
  }
  if (!style.properties) {
    return;
  }
  const auto found = style.properties->unparsedProperties.find(RcString("flood-opacity"));
  if (found == style.properties->unparsedProperties.end()) {
    return;
  }
  const auto params =
      parser::PropertyParseFnParams::Create(found->second.declaration, found->second.specificity,
                                            parser::PropertyParseBehavior::AllowUserUnits);
  if (auto error = Parse(
          params,
          [](const parser::PropertyParseFnParams& input) {
            return parser::ParseAlphaValue(input.components());
          },
          &opacity)) {
    warnings.add(std::move(*error));
  }
}

template <typename Component>
void ApplyFloodStyle(Component& properties, const ComputedStyleComponent& style,
                     ParseWarningSink& warnings) {
  ApplyFloodOpacity(properties.floodOpacity, style, warnings);
  if (!style.properties) {
    return;
  }
  const auto& color = style.properties->floodColor;
  if (color.isSpecified() && color.specificity >= properties.floodColor.specificity) {
    properties.floodColor = color;
  }
  if (properties.floodColor.isSpecified() && properties.floodColor.get().value().isCurrentColor()) {
    const auto& currentColor = style.properties->color;
    properties.floodColor.set(currentColor.get().value(), currentColor.specificity);
  }
}

template <typename Component>
filter_primitive::Flood ResolveFloodPaint(const Registry& registry, Entity entity,
                                          Component properties, ParseWarningSink& warnings) {
  if (const auto* style = registry.try_get<ComputedStyleComponent>(entity)) {
    ApplyFloodStyle(properties, *style, warnings);
  }
  filter_primitive::Flood result;
  if (properties.floodColor.isSpecified()) {
    result.floodColor = properties.floodColor.get().value();
  }
  if (properties.floodOpacity.isSpecified()) {
    result.floodOpacity = properties.floodOpacity.get().value();
  }
  return result;
}

/// Resolve lighting-color from component and computed-style values in cascade priority order.
template <typename LightingComponent>
css::Color resolveLightingColor(const Registry& registry, entt::entity cur,
                                ParseWarningSink& warningSink) {
  Property<css::Color> lightingColor{"lighting-color", []() -> std::optional<css::Color> {
                                       return css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF));
                                     }};
  if (const auto* comp = registry.try_get<LightingComponent>(cur)) {
    lightingColor = comp->lightingColor;
  }

  // Overlay computed CSS properties (from attributes, inline styles, or stylesheet rules).
  if (const auto* style = registry.try_get<ComputedStyleComponent>(cur)) {
    if (style->properties.has_value()) {
      const auto& computedColor = style->properties->lightingColor;
      if (computedColor.isSpecified() && computedColor.specificity >= lightingColor.specificity) {
        lightingColor = computedColor;
      }
      for (const auto& [name, unparsedProperty] : style->properties->unparsedProperties) {
        if (name == "lighting-color") {
          const parser::PropertyParseFnParams params = parser::PropertyParseFnParams::Create(
              unparsedProperty.declaration, unparsedProperty.specificity,
              parser::PropertyParseBehavior::AllowUserUnits);

          if (auto maybeError = Parse(
                  params,
                  [](const parser::PropertyParseFnParams& params) {
                    return css::parser::ColorParser::Parse(params.components());
                  },
                  &lightingColor)) {
            warningSink.add(std::move(maybeError.value()));
          }
        }
      }
    }

    // Resolve currentColor.
    if (lightingColor.isSpecified() && lightingColor.get().value().isCurrentColor()) {
      const auto& currentColor = style->properties->color;
      lightingColor.set(currentColor.get().value(), currentColor.specificity);
    }
  }

  return lightingColor.get().value();
}

/// Resolve color-interpolation-filters on an individual filter primitive entity.
/// Returns the per-primitive value if explicitly set, or std::nullopt if the filter-level
/// default should be used.
std::optional<ColorInterpolationFilters> resolveColorInterpolationFilters(const Registry& registry,
                                                                          entt::entity cur) {
  if (const auto* style = registry.try_get<ComputedStyleComponent>(cur)) {
    if (style->properties.has_value() &&
        style->properties->colorInterpolationFilters.isSpecified()) {
      return style->properties->colorInterpolationFilters.get().value();
    }
  }
  return std::nullopt;
}

template <typename Component>
const Component* ActiveFilterComponent(const Registry& registry, Entity entity) {
  return HasSvgNamespace(registry, entity) ? registry.try_get<Component>(entity) : nullptr;
}

bool hasFilterPrimitiveChildren(const Registry& registry, EntityHandle handle) {
  const auto& tree = handle.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<donner::components::TreeComponent>(cur).nextSibling()) {
    if (HasSvgNamespace(registry, cur) &&
        registry.try_get<FilterPrimitiveComponent>(cur) != nullptr) {
      return true;
    }
  }

  return false;
}

std::vector<EntityHandle> getInheritanceChain(EntityHandle handle, ParseWarningSink& warningSink) {
  Registry& registry = *handle.registry();
  auto& budget = GetReferenceResolutionBudget(registry);

  std::vector<EntityHandle> inheritanceChain;
  inheritanceChain.push_back(handle);

  RecursionGuard guard;
  guard.add(handle);

  EntityHandle current = handle;
  while (const auto* filter = current.try_get<FilterComponent>()) {
    if (!filter->href.has_value()) {
      break;
    }
    const bool alreadyRejected = budget.stats(ReferenceResolutionBudget::Kind::Filter).rejected;
    if (!budget.reserve(ReferenceResolutionBudget::Kind::Filter, inheritanceChain.size())) {
      if (!alreadyRejected) {
        warningSink.add(ParseDiagnostic::Warning("Filter inheritance resource limit exceeded",
                                                 FileOffset::Offset(0)));
      }
      return {};
    }

    auto resolvedReference = filter->href->resolve(registry);
    if (!resolvedReference.has_value()) {
      ParseDiagnostic err;
      err.reason = "Filter element href=\"" + filter->href->href + "\" failed to resolve";
      warningSink.add(std::move(err));
      break;
    }

    EntityHandle target = resolvedReference->handle;
    if (!target.valid() || !target.all_of<FilterComponent>()) {
      ParseDiagnostic err;
      err.reason =
          "Filter element href=\"" + filter->href->href + "\" does not reference a <filter>";
      warningSink.add(std::move(err));
      break;
    }

    if (guard.hasRecursion(target)) {
      ParseDiagnostic err;
      err.reason = "Circular filter inheritance detected";
      warningSink.add(std::move(err));
      break;
    }

    inheritanceChain.push_back(target);
    guard.add(target);
    current = target;
  }

  return inheritanceChain;
}

Entity NextFilterChild(Registry& registry, Entity current, std::size_t retainedNodeCount) {
  if (retainedNodeCount >= kMaximumFilterGraphNodes) {
    return entt::null;
  }
  return registry.get<donner::components::TreeComponent>(current).nextSibling();
}

void AppendColorMatrixNode(FilterGraph& graph, const FEColorMatrixComponent& component,
                           const FilterPrimitiveComponent& attributes,
                           std::optional<ColorInterpolationFilters> colorInterpolation) {
  filter_primitive::ColorMatrix primitive;
  primitive.type = static_cast<filter_primitive::ColorMatrix::Type>(component.type);
  if (component.values.size() <= kMaximumFilterColorMatrixValues) {
    primitive.values = component.values;
  }
  graph.nodes.push_back(makeFilterNode(std::move(primitive), attributes, colorInterpolation));
}

void AppendComponentTransferNode(Registry& registry, Entity current, FilterGraph& graph,
                                 const FilterPrimitiveComponent& attributes,
                                 std::optional<ColorInterpolationFilters> colorInterpolation) {
  filter_primitive::ComponentTransfer primitive;
  const auto& tree = registry.get<donner::components::TreeComponent>(current);
  for (Entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    const auto* function = ActiveFilterComponent<FEFuncComponent>(registry, child);
    if (!function || function->tableValues.size() > kMaximumFilterTableValues) {
      continue;
    }
    filter_primitive::ComponentTransfer::Func value;
    value.type = static_cast<filter_primitive::ComponentTransfer::FuncType>(function->type);
    value.tableValues = function->tableValues;
    value.slope = function->slope;
    value.intercept = function->intercept;
    value.amplitude = function->amplitude;
    value.exponent = function->exponent;
    value.offset = function->offset;
    switch (function->channel) {
      case FEFuncComponent::Channel::R: primitive.funcR = std::move(value); break;
      case FEFuncComponent::Channel::G: primitive.funcG = std::move(value); break;
      case FEFuncComponent::Channel::B: primitive.funcB = std::move(value); break;
      case FEFuncComponent::Channel::A: primitive.funcA = std::move(value); break;
    }
  }
  graph.nodes.push_back(makeFilterNode(std::move(primitive), attributes, colorInterpolation));
}

void AppendMergeNode(Registry& registry, Entity current, FilterGraph& graph,
                     const FilterPrimitiveComponent& attributes,
                     std::optional<ColorInterpolationFilters> colorInterpolation) {
  FilterNode node;
  node.primitive = filter_primitive::Merge{};
  node.result = attributes.result;
  node.x = attributes.x;
  node.y = attributes.y;
  node.width = attributes.width;
  node.height = attributes.height;
  node.colorInterpolationFilters = colorInterpolation;
  const auto& tree = registry.get<donner::components::TreeComponent>(current);
  for (Entity child = tree.firstChild();
       child != entt::null && node.inputs.size() < kMaximumFilterMergeInputs;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    if (const auto* mergeNode = ActiveFilterComponent<FEMergeNodeComponent>(registry, child)) {
      node.inputs.push_back(mergeNode->in.value_or(FilterInput{FilterInput::Previous{}}));
    }
  }
  graph.nodes.push_back(std::move(node));
}

void AppendConvolveNode(FilterGraph& graph, const FEConvolveMatrixComponent& component,
                        const FilterPrimitiveComponent& attributes,
                        std::optional<ColorInterpolationFilters> colorInterpolation) {
  if (component.orderX <= 0 || component.orderY <= 0 ||
      component.orderX > kMaximumFilterConvolveOrder ||
      component.orderY > kMaximumFilterConvolveOrder ||
      component.kernelMatrix.size() > kMaximumFilterKernelValues) {
    return;
  }
  filter_primitive::ConvolveMatrix primitive;
  primitive.orderX = component.orderX;
  primitive.orderY = component.orderY;
  primitive.kernelMatrix = component.kernelMatrix;
  primitive.divisor = component.divisor;
  primitive.bias = component.bias;
  primitive.targetX = component.targetX;
  primitive.targetY = component.targetY;
  primitive.edgeMode = static_cast<filter_primitive::ConvolveMatrix::EdgeMode>(component.edgeMode);
  primitive.preserveAlpha = component.preserveAlpha;
  graph.nodes.push_back(makeFilterNode(std::move(primitive), attributes, colorInterpolation));
}

void AppendImageNode(Registry& registry, Entity current, FilterGraph& graph,
                     const FEImageComponent& component, const FilterPrimitiveComponent& attributes,
                     std::optional<ColorInterpolationFilters> colorInterpolation,
                     ComputedFilterResourceBudget& budget) {
  filter_primitive::Image primitive;
  primitive.href = component.href;
  primitive.preserveAspectRatio = component.preserveAspectRatio;
  if (const auto* style = registry.try_get<ComputedStyleComponent>(current);
      style && style->properties.has_value()) {
    primitive.imageRendering = style->properties->imageRendering.get().value();
  }
  if (const auto* loaded = registry.try_get<LoadedImageComponent>(current);
      loaded && loaded->image.has_value()) {
    primitive.imageData = budget.shareImage(current, loaded->revision(), loaded->image->data);
    if (!primitive.imageData) {
      return;
    }
    primitive.imageWidth = loaded->image->width;
    primitive.imageHeight = loaded->image->height;
  } else if (const auto* svgLoaded = registry.try_get<LoadedSVGImageComponent>(current);
             svgLoaded && svgLoaded->subDocument) {
    primitive.svgSubDocument = svgLoaded->subDocument;
  } else if (const std::string_view hrefView(component.href);
             !hrefView.empty() && hrefView[0] == '#') {
    primitive.fragmentId = RcString(hrefView.substr(1));
    primitive.isFragmentReference = true;
  }
  FilterNode node;
  node.primitive = std::move(primitive);
  node.result = attributes.result;
  node.x = attributes.x;
  node.y = attributes.y;
  node.width = attributes.width;
  node.height = attributes.height;
  node.colorInterpolationFilters = colorInterpolation;
  graph.nodes.push_back(std::move(node));
}

void AttachAnimatedRegion(Registry& registry, Entity primitive, FilterGraph& graph,
                          std::size_t previousSize) {
  const auto* values = registry.try_get<AnimatedValuesComponent>(primitive);
  if (!values || graph.nodes.size() == previousSize) {
    return;
  }
  const std::array<std::string_view, 4> names = {"x", "y", "width", "height"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto found = values->contextualLengths.find(std::string(names[i]));
    if (found != values->contextualLengths.end()) {
      graph.nodes.back().animatedRegion[i] = found->second;
    }
  }
}

void StoreComputedFilter(EntityHandle handle, ComputedFilterResourceBudget& budget,
                         std::vector<FilterEffect> effects, FilterGraph graph, Lengthd x, Lengthd y,
                         Lengthd width, Lengthd height, FilterUnits filterUnits,
                         PrimitiveUnits primitiveUnits,
                         ColorInterpolationFilters colorInterpolation) {
  if (budget.rejected()) {
    RemoveComputedFilter(handle);
    return;
  }
  const auto retainedBytes = ComputedFilterRetainedBytes(graph, effects);
  if (!retainedBytes.has_value() || !budget.reserve(handle.entity(), *retainedBytes)) {
    RemoveComputedFilter(handle);
    return;
  }
  ComputedFilterComponent& computed = handle.emplace_or_replace<ComputedFilterComponent>();
  computed.effectChain = std::move(effects);
  computed.filterGraph = std::move(graph);
  computed.x = x;
  computed.y = y;
  computed.width = width;
  computed.height = height;
  computed.filterUnits = filterUnits;
  computed.primitiveUnits = primitiveUnits;
  computed.colorInterpolationFilters = colorInterpolation;
  computed.filterGraph.colorInterpolationFilters = colorInterpolation;
  computed.filterGraph.primitiveUnits = primitiveUnits;
}

/// Borrowed inputs for synchronous primitive dispatch; no builder retains this context.
struct PrimitiveBuildContext {
  Registry& registry;
  Entity entity;
  FilterGraph& graph;
  std::vector<FilterEffect>& effects;
  const FilterPrimitiveComponent& attributes;
  std::optional<ColorInterpolationFilters> colorInterpolation;
  ComputedFilterResourceBudget& budget;
  ParseWarningSink& warnings;
};

std::optional<filter_primitive::LightSource> FindLightSource(const Registry& registry,
                                                             Entity entity) {
  const auto& tree = registry.get<donner::components::TreeComponent>(entity);
  for (Entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    if (const auto* component = ActiveFilterComponent<LightSourceComponent>(registry, child)) {
      filter_primitive::LightSource light;
      light.type = static_cast<filter_primitive::LightSource::Type>(component->type);
      light.azimuth = component->azimuth;
      light.elevation = component->elevation;
      light.x = component->x;
      light.y = component->y;
      light.z = component->z;
      light.pointsAtX = component->pointsAtX;
      light.pointsAtY = component->pointsAtY;
      light.pointsAtZ = component->pointsAtZ;
      light.spotExponent = component->spotExponent;
      light.limitingConeAngle = component->limitingConeAngle;
      return light;
    }
  }
  return std::nullopt;
}

void AppendPrimitive(const PrimitiveBuildContext& context,
                     const FEGaussianBlurComponent& component) {
  context.effects.emplace_back(FilterEffect::Blur{
      .stdDeviationX = Lengthd(component.stdDeviationX),
      .stdDeviationY = Lengthd(component.stdDeviationY),
  });

  context.graph.nodes.push_back(makeFilterNode(
      filter_primitive::GaussianBlur{
          .stdDeviationX = component.stdDeviationX,
          .stdDeviationY = component.stdDeviationY,
          .edgeMode = static_cast<filter_primitive::GaussianBlur::EdgeMode>(component.edgeMode),
      },
      context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEFloodComponent& component) {
  context.graph.nodes.push_back(makeFilterNode(
      ResolveFloodPaint(context.registry, context.entity, component, context.warnings),
      context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEOffsetComponent& component) {
  context.graph.nodes.push_back(makeFilterNode(
      filter_primitive::Offset{
          .dx = component.dx,
          .dy = component.dy,
      },
      context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FECompositeComponent& component) {
  filter_primitive::Composite prim;
  prim.op = static_cast<filter_primitive::Composite::Operator>(component.op);
  prim.k1 = component.k1;
  prim.k2 = component.k2;
  prim.k3 = component.k3;
  prim.k4 = component.k4;
  context.graph.nodes.push_back(
      makeFilterNode2(std::move(prim), context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context,
                     const FEColorMatrixComponent& component) {
  AppendColorMatrixNode(context.graph, component, context.attributes, context.colorInterpolation);
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEBlendComponent& component) {
  filter_primitive::Blend prim;
  prim.mode = static_cast<filter_primitive::Blend::Mode>(component.mode);
  context.graph.nodes.push_back(
      makeFilterNode2(std::move(prim), context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEComponentTransferComponent&) {
  AppendComponentTransferNode(context.registry, context.entity, context.graph, context.attributes,
                              context.colorInterpolation);
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEMergeComponent&) {
  AppendMergeNode(context.registry, context.entity, context.graph, context.attributes,
                  context.colorInterpolation);
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEDropShadowComponent& component) {
  filter_primitive::DropShadow prim;
  prim.dx = component.dx;
  prim.dy = component.dy;
  prim.stdDeviationX = component.stdDeviationX;
  prim.stdDeviationY = component.stdDeviationY;
  const auto paint =
      ResolveFloodPaint(context.registry, context.entity, component, context.warnings);
  prim.floodColor = paint.floodColor;
  prim.floodOpacity = paint.floodOpacity;
  context.graph.nodes.push_back(
      makeFilterNode(prim, context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEMorphologyComponent& component) {
  filter_primitive::Morphology prim;
  prim.op = static_cast<filter_primitive::Morphology::Operator>(component.op);
  prim.radiusX = component.radiusX;
  prim.radiusY = component.radiusY;
  context.graph.nodes.push_back(
      makeFilterNode(prim, context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context,
                     const FEConvolveMatrixComponent& component) {
  AppendConvolveNode(context.graph, component, context.attributes, context.colorInterpolation);
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FETileComponent&) {
  context.graph.nodes.push_back(
      makeFilterNode(filter_primitive::Tile{}, context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FETurbulenceComponent& component) {
  filter_primitive::Turbulence prim;
  prim.type = static_cast<filter_primitive::Turbulence::Type>(component.type);
  prim.baseFrequencyX = component.baseFrequencyX;
  prim.baseFrequencyY = component.baseFrequencyY;
  prim.numOctaves = component.numOctaves;
  prim.seed = component.seed;
  prim.stitchTiles = component.stitchTiles;
  context.graph.nodes.push_back(
      makeFilterNode(prim, context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context,
                     const FEDisplacementMapComponent& component) {
  filter_primitive::DisplacementMap prim;
  prim.scale = component.scale;
  prim.xChannelSelector =
      static_cast<filter_primitive::DisplacementMap::Channel>(component.xChannelSelector);
  prim.yChannelSelector =
      static_cast<filter_primitive::DisplacementMap::Channel>(component.yChannelSelector);
  context.graph.nodes.push_back(
      makeFilterNode2(prim, context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context,
                     const FEDiffuseLightingComponent& component) {
  filter_primitive::DiffuseLighting prim;
  prim.surfaceScale = component.surfaceScale;
  prim.diffuseConstant = component.diffuseConstant;
  prim.lightingColor = resolveLightingColor<FEDiffuseLightingComponent>(
      context.registry, context.entity, context.warnings);

  prim.light = FindLightSource(context.registry, context.entity);

  context.graph.nodes.push_back(
      makeFilterNode(std::move(prim), context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context,
                     const FESpecularLightingComponent& component) {
  filter_primitive::SpecularLighting prim;
  prim.surfaceScale = component.surfaceScale;
  prim.specularConstant = component.specularConstant;
  prim.specularExponent = component.specularExponent;
  prim.lightingColor = resolveLightingColor<FESpecularLightingComponent>(
      context.registry, context.entity, context.warnings);

  prim.light = FindLightSource(context.registry, context.entity);

  context.graph.nodes.push_back(
      makeFilterNode(std::move(prim), context.attributes, context.colorInterpolation));
}

void AppendPrimitive(const PrimitiveBuildContext& context, const FEImageComponent& component) {
  AppendImageNode(context.registry, context.entity, context.graph, component, context.attributes,
                  context.colorInterpolation, context.budget);
}

template <typename Component>
bool TryAppendPrimitive(const PrimitiveBuildContext& context) {
  if (const auto* component = context.registry.try_get<Component>(context.entity)) {
    AppendPrimitive(context, *component);
    return true;
  }
  return false;
}

void AppendPrimitiveNode(const PrimitiveBuildContext& context) {
  using Builder = bool (*)(const PrimitiveBuildContext&);
  static constexpr std::array<Builder, 17> builders = {
      &TryAppendPrimitive<FEGaussianBlurComponent>,
      &TryAppendPrimitive<FEFloodComponent>,
      &TryAppendPrimitive<FEOffsetComponent>,
      &TryAppendPrimitive<FECompositeComponent>,
      &TryAppendPrimitive<FEColorMatrixComponent>,
      &TryAppendPrimitive<FEBlendComponent>,
      &TryAppendPrimitive<FEComponentTransferComponent>,
      &TryAppendPrimitive<FEMergeComponent>,
      &TryAppendPrimitive<FEDropShadowComponent>,
      &TryAppendPrimitive<FEMorphologyComponent>,
      &TryAppendPrimitive<FEConvolveMatrixComponent>,
      &TryAppendPrimitive<FETileComponent>,
      &TryAppendPrimitive<FETurbulenceComponent>,
      &TryAppendPrimitive<FEDisplacementMapComponent>,
      &TryAppendPrimitive<FEDiffuseLightingComponent>,
      &TryAppendPrimitive<FESpecularLightingComponent>,
      &TryAppendPrimitive<FEImageComponent>,
  };
  for (const auto build : builders) {
    if (build(context)) {
      return;
    }
  }
}

}  // namespace

void FilterSystem::createComputedFilter(EntityHandle handle, const FilterComponent& component,
                                        ParseWarningSink& warningSink) {
  (void)component;

  Registry& registry = *handle.registry();
  auto& computedBudget = GetComputedFilterResourceBudget(registry);

  const std::vector<EntityHandle> inheritanceChain = getInheritanceChain(handle, warningSink);
  if (inheritanceChain.empty()) {
    RemoveComputedFilter(handle);
    return;
  }

  EntityHandle primitiveSource;
  for (EntityHandle candidate : inheritanceChain) {
    if (hasFilterPrimitiveChildren(registry, candidate)) {
      primitiveSource = candidate;
      break;
    }
  }

  if (!primitiveSource.valid()) {
    primitiveSource = handle;
  }

  Lengthd computedX(-10.0, Lengthd::Unit::Percent);
  Lengthd computedY(-10.0, Lengthd::Unit::Percent);
  Lengthd computedWidth(120.0, Lengthd::Unit::Percent);
  Lengthd computedHeight(120.0, Lengthd::Unit::Percent);
  FilterUnits computedFilterUnits = FilterUnits::Default;
  PrimitiveUnits computedPrimitiveUnits = PrimitiveUnits::Default;
  ColorInterpolationFilters computedColorInterpolationFilters = ColorInterpolationFilters::Default;

  for (auto it = inheritanceChain.rbegin(); it != inheritanceChain.rend(); ++it) {
    const FilterComponent& current = it->get<FilterComponent>();
    computedX = current.x.value_or(computedX);
    computedY = current.y.value_or(computedY);
    computedWidth = current.width.value_or(computedWidth);
    computedHeight = current.height.value_or(computedHeight);
    computedFilterUnits = current.filterUnits.value_or(computedFilterUnits);
    computedPrimitiveUnits = current.primitiveUnits.value_or(computedPrimitiveUnits);
    computedColorInterpolationFilters =
        current.colorInterpolationFilters.value_or(computedColorInterpolationFilters);
  }

  std::vector<FilterEffect> effectChain;
  FilterGraph filterGraph;

  // Find the first filter in the inheritance chain that contributes primitive children.
  const donner::components::TreeComponent& tree =
      primitiveSource.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = NextFilterChild(registry, cur, filterGraph.nodes.size())) {
    const auto* primitive = ActiveFilterComponent<FilterPrimitiveComponent>(registry, cur);
    if (!primitive) {
      continue;
    }

    // Resolve per-primitive color-interpolation-filters (overrides filter-level default).
    const auto primitiveCIF = resolveColorInterpolationFilters(registry, cur);

    const std::size_t previousNodeCount = filterGraph.nodes.size();
    // Determine which filter primitive we have.
    AppendPrimitiveNode({registry, cur, filterGraph, effectChain, *primitive, primitiveCIF,
                         computedBudget, warningSink});
    AttachAnimatedRegion(registry, cur, filterGraph, previousNodeCount);
  }

  StoreComputedFilter(handle, computedBudget, std::move(effectChain), std::move(filterGraph),
                      computedX, computedY, computedWidth, computedHeight, computedFilterUnits,
                      computedPrimitiveUnits, computedColorInterpolationFilters);
}

void FilterSystem::instantiateAllComputedComponents(Registry& registry,
                                                    ParseWarningSink& warningSink) {
  GetReferenceResolutionBudget(registry).reset(ReferenceResolutionBudget::Kind::Filter);
  GetComputedFilterResourceBudget(registry);
  for (auto entity : registry.view<FilterComponent>()) {
    if (!HasSvgNamespace(registry, entity)) {
      RemoveComputedFilter(EntityHandle(registry, entity));
      continue;
    }
    createComputedFilter(EntityHandle(registry, entity), registry.get<FilterComponent>(entity),
                         warningSink);
  }
}

}  // namespace donner::svg::components
