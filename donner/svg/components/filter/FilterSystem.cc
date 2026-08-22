#include "donner/svg/components/filter/FilterSystem.h"

#include <limits>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ReferenceResolutionBudget.h"
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

std::optional<std::size_t> ComputedFilterRetainedBytes(
    const FilterGraph& graph, const std::vector<FilterEffect>& effectChain) {
  std::size_t bytes = sizeof(ComputedFilterComponent);
  if (!AddRetainedProduct(bytes, effectChain.capacity(), sizeof(FilterEffect)) ||
      !AddRetainedProduct(bytes, graph.nodes.capacity(), sizeof(FilterNode))) {
    return std::nullopt;
  }
  for (const FilterNode& node : graph.nodes) {
    if (!AddRetainedProduct(bytes, node.inputs.capacity(), sizeof(FilterInput)) ||
        !AddRetainedBytes(bytes,
                          static_cast<std::size_t>(FilterPrimitivePayloadBytes(node.primitive)))) {
      return std::nullopt;
    }
    for (const FilterInput& input : node.inputs) {
      if (const auto* named = std::get_if<FilterInput::Named>(&input.value);
          named && !AddRetainedBytes(bytes, named->name.size())) {
        return std::nullopt;
      }
    }
    if (node.result.has_value() && !AddRetainedBytes(bytes, node.result->size())) {
      return std::nullopt;
    }
    if (const auto* image = std::get_if<filter_primitive::Image>(&node.primitive)) {
      if (!AddRetainedBytes(bytes, image->href.size()) ||
          !AddRetainedBytes(bytes, image->fragmentId.size())) {
        return std::nullopt;
      }
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

/// Resolve flood-color and flood-opacity from the FEFloodComponent properties overlaid with CSS
/// unparsed properties, and return a Flood primitive with the resolved values.
filter_primitive::Flood resolveFloodProperties(const Registry& registry, entt::entity cur,
                                               ParseWarningSink& warningSink) {
  filter_primitive::Flood flood;

  // Start with values from the component (set via XML presentation attributes).
  FEFloodComponent props;
  if (const auto* comp = registry.try_get<FEFloodComponent>(cur)) {
    props = *comp;
  }

  // Overlay CSS unparsed properties (from style="" or stylesheet rules).
  if (const auto* style = registry.try_get<ComputedStyleComponent>(cur)) {
    if (style->properties.has_value()) {
      for (const auto& [name, unparsedProperty] : style->properties->unparsedProperties) {
        const parser::PropertyParseFnParams params = parser::PropertyParseFnParams::Create(
            unparsedProperty.declaration, unparsedProperty.specificity,
            parser::PropertyParseBehavior::AllowUserUnits);

        if (name == "flood-color") {
          if (auto maybeError = Parse(
                  params,
                  [](const parser::PropertyParseFnParams& params) {
                    return css::parser::ColorParser::Parse(params.components());
                  },
                  &props.floodColor)) {
            warningSink.add(std::move(maybeError.value()));
          }
        } else if (name == "flood-opacity") {
          if (auto maybeError = Parse(
                  params,
                  [](const parser::PropertyParseFnParams& params) {
                    return parser::ParseAlphaValue(params.components());
                  },
                  &props.floodOpacity)) {
            warningSink.add(std::move(maybeError.value()));
          }
        }
      }
    }

    // Resolve currentColor.
    if (props.floodColor.isSpecified() && props.floodColor.get().value().isCurrentColor()) {
      const auto& currentColor = style->properties->color;
      props.floodColor.set(currentColor.get().value(), currentColor.specificity);
    }
  }

  if (props.floodColor.isSpecified()) {
    flood.floodColor = props.floodColor.get().value();
  }
  if (props.floodOpacity.isSpecified()) {
    flood.floodOpacity = props.floodOpacity.get().value();
  }

  return flood;
}

/// Resolve lighting-color from a lighting component's Property, overlaid with CSS unparsed
/// properties, following the same pattern as resolveFloodProperties for flood-color.
template <typename LightingComponent>
css::Color resolveLightingColor(const Registry& registry, entt::entity cur,
                                ParseWarningSink& warningSink) {
  // Start with values from the component (set via XML presentation attributes).
  Property<css::Color> lightingColor{"lighting-color", []() -> std::optional<css::Color> {
                                       return css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF));
                                     }};
  if (const auto* comp = registry.try_get<LightingComponent>(cur)) {
    lightingColor = comp->lightingColor;
  }

  // Overlay CSS unparsed properties (from style="" or stylesheet rules).
  if (const auto* style = registry.try_get<ComputedStyleComponent>(cur)) {
    if (style->properties.has_value()) {
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

bool hasFilterPrimitiveChildren(const Registry& registry, EntityHandle handle) {
  const auto& tree = handle.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<donner::components::TreeComponent>(cur).nextSibling()) {
    if (registry.try_get<FilterPrimitiveComponent>(cur) != nullptr) {
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
      break;
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
    const auto* function = registry.try_get<FEFuncComponent>(child);
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
    if (const auto* mergeNode = registry.try_get<FEMergeNodeComponent>(child)) {
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

void StoreComputedFilter(EntityHandle handle, ComputedFilterResourceBudget& budget,
                         std::vector<FilterEffect> effects, FilterGraph graph, Lengthd x, Lengthd y,
                         Lengthd width, Lengthd height, FilterUnits filterUnits,
                         PrimitiveUnits primitiveUnits,
                         ColorInterpolationFilters colorInterpolation) {
  if (budget.rejected()) {
    RemoveComputedFilter(handle);
    return;
  }
  if (effects.empty() && graph.empty()) {
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

}  // namespace

void FilterSystem::createComputedFilter(EntityHandle handle, const FilterComponent& component,
                                        ParseWarningSink& warningSink) {
  (void)component;

  Registry& registry = *handle.registry();
  auto& computedBudget = GetComputedFilterResourceBudget(registry);

  const std::vector<EntityHandle> inheritanceChain = getInheritanceChain(handle, warningSink);

  EntityHandle primitiveSource;
  for (EntityHandle candidate : inheritanceChain) {
    if (hasFilterPrimitiveChildren(registry, candidate)) {
      primitiveSource = candidate;
      break;
    }
  }

  if (!primitiveSource.valid()) {
    RemoveComputedFilter(handle);
    return;
  }

  Lengthd computedX(-10.0, Lengthd::Unit::Percent);
  Lengthd computedY(-10.0, Lengthd::Unit::Percent);
  Lengthd computedWidth(120.0, Lengthd::Unit::Percent);
  Lengthd computedHeight(120.0, Lengthd::Unit::Percent);
  FilterUnits computedFilterUnits = FilterUnits::Default;
  PrimitiveUnits computedPrimitiveUnits = PrimitiveUnits::Default;
  ColorInterpolationFilters computedColorInterpolationFilters = ColorInterpolationFilters::Default;

  for (auto it = inheritanceChain.rbegin(); it != inheritanceChain.rend(); ++it) {
    const FilterComponent& currentFilter = it->get<FilterComponent>();
    if (currentFilter.x.has_value()) {
      computedX = *currentFilter.x;
    }
    if (currentFilter.y.has_value()) {
      computedY = *currentFilter.y;
    }
    if (currentFilter.width.has_value()) {
      computedWidth = *currentFilter.width;
    }
    if (currentFilter.height.has_value()) {
      computedHeight = *currentFilter.height;
    }
    if (currentFilter.filterUnits.has_value()) {
      computedFilterUnits = *currentFilter.filterUnits;
    }
    if (currentFilter.primitiveUnits.has_value()) {
      computedPrimitiveUnits = *currentFilter.primitiveUnits;
    }
    if (currentFilter.colorInterpolationFilters.has_value()) {
      computedColorInterpolationFilters = *currentFilter.colorInterpolationFilters;
    }
  }

  std::vector<FilterEffect> effectChain;
  FilterGraph filterGraph;

  // Find the first filter in the inheritance chain that contributes primitive children.
  const donner::components::TreeComponent& tree =
      primitiveSource.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = NextFilterChild(registry, cur, filterGraph.nodes.size())) {
    const auto* primitive = registry.try_get<FilterPrimitiveComponent>(cur);
    if (!primitive) {
      continue;
    }

    // Resolve per-primitive color-interpolation-filters (overrides filter-level default).
    const auto primitiveCIF = resolveColorInterpolationFilters(registry, cur);

    // Determine which filter primitive we have.
    if (const auto* blur = registry.try_get<FEGaussianBlurComponent>(cur)) {
      // Legacy effectChain for backward-compat.
      effectChain.emplace_back(FilterEffect::Blur{
          .stdDeviationX = Lengthd(blur->stdDeviationX),
          .stdDeviationY = Lengthd(blur->stdDeviationY),
      });

      filterGraph.nodes.push_back(makeFilterNode(
          filter_primitive::GaussianBlur{
              .stdDeviationX = blur->stdDeviationX,
              .stdDeviationY = blur->stdDeviationY,
              .edgeMode = static_cast<filter_primitive::GaussianBlur::EdgeMode>(blur->edgeMode),
          },
          *primitive, primitiveCIF));
    } else if (registry.try_get<FEFloodComponent>(cur)) {
      filterGraph.nodes.push_back(makeFilterNode(resolveFloodProperties(registry, cur, warningSink),
                                                 *primitive, primitiveCIF));
    } else if (const auto* offset = registry.try_get<FEOffsetComponent>(cur)) {
      filterGraph.nodes.push_back(makeFilterNode(
          filter_primitive::Offset{
              .dx = offset->dx,
              .dy = offset->dy,
          },
          *primitive, primitiveCIF));
    } else if (const auto* comp = registry.try_get<FECompositeComponent>(cur)) {
      filter_primitive::Composite prim;
      prim.op = static_cast<filter_primitive::Composite::Operator>(comp->op);
      prim.k1 = comp->k1;
      prim.k2 = comp->k2;
      prim.k3 = comp->k3;
      prim.k4 = comp->k4;
      filterGraph.nodes.push_back(makeFilterNode2(std::move(prim), *primitive, primitiveCIF));
    } else if (const auto* colorMatrix = registry.try_get<FEColorMatrixComponent>(cur)) {
      AppendColorMatrixNode(filterGraph, *colorMatrix, *primitive, primitiveCIF);
    } else if (const auto* blend = registry.try_get<FEBlendComponent>(cur)) {
      filter_primitive::Blend prim;
      prim.mode = static_cast<filter_primitive::Blend::Mode>(blend->mode);
      filterGraph.nodes.push_back(makeFilterNode2(std::move(prim), *primitive, primitiveCIF));
    } else if (registry.try_get<FEComponentTransferComponent>(cur)) {
      AppendComponentTransferNode(registry, cur, filterGraph, *primitive, primitiveCIF);
    } else if (registry.try_get<FEMergeComponent>(cur)) {
      AppendMergeNode(registry, cur, filterGraph, *primitive, primitiveCIF);
    } else if (const auto* dropShadow = registry.try_get<FEDropShadowComponent>(cur)) {
      filter_primitive::DropShadow prim;
      prim.dx = dropShadow->dx;
      prim.dy = dropShadow->dy;
      prim.stdDeviationX = dropShadow->stdDeviationX;
      prim.stdDeviationY = dropShadow->stdDeviationY;

      // Resolve flood-color and flood-opacity from component + CSS properties.
      FEDropShadowComponent props = *dropShadow;
      if (const auto* style = registry.try_get<ComputedStyleComponent>(cur)) {
        if (style->properties.has_value()) {
          for (const auto& [name, unparsedProperty] : style->properties->unparsedProperties) {
            const parser::PropertyParseFnParams params = parser::PropertyParseFnParams::Create(
                unparsedProperty.declaration, unparsedProperty.specificity,
                parser::PropertyParseBehavior::AllowUserUnits);

            if (name == "flood-color") {
              if (auto maybeError = Parse(
                      params,
                      [](const parser::PropertyParseFnParams& params) {
                        return css::parser::ColorParser::Parse(params.components());
                      },
                      &props.floodColor)) {
                warningSink.add(std::move(maybeError.value()));
              }
            } else if (name == "flood-opacity") {
              if (auto maybeError = Parse(
                      params,
                      [](const parser::PropertyParseFnParams& params) {
                        return parser::ParseAlphaValue(params.components());
                      },
                      &props.floodOpacity)) {
                warningSink.add(std::move(maybeError.value()));
              }
            }
          }
        }

        // Resolve currentColor.
        if (props.floodColor.isSpecified() && props.floodColor.get().value().isCurrentColor()) {
          const auto& currentColor = style->properties->color;
          props.floodColor.set(currentColor.get().value(), currentColor.specificity);
        }
      }

      if (props.floodColor.isSpecified()) {
        prim.floodColor = props.floodColor.get().value();
      }
      if (props.floodOpacity.isSpecified()) {
        prim.floodOpacity = props.floodOpacity.get().value();
      }

      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
    } else if (const auto* morph = registry.try_get<FEMorphologyComponent>(cur)) {
      filter_primitive::Morphology prim;
      prim.op = static_cast<filter_primitive::Morphology::Operator>(morph->op);
      prim.radiusX = morph->radiusX;
      prim.radiusY = morph->radiusY;
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
    } else if (const auto* convolve = registry.try_get<FEConvolveMatrixComponent>(cur)) {
      AppendConvolveNode(filterGraph, *convolve, *primitive, primitiveCIF);
    } else if (registry.try_get<FETileComponent>(cur)) {
      filterGraph.nodes.push_back(
          makeFilterNode(filter_primitive::Tile{}, *primitive, primitiveCIF));
    } else if (const auto* turbulence = registry.try_get<FETurbulenceComponent>(cur)) {
      filter_primitive::Turbulence prim;
      prim.type = static_cast<filter_primitive::Turbulence::Type>(turbulence->type);
      prim.baseFrequencyX = turbulence->baseFrequencyX;
      prim.baseFrequencyY = turbulence->baseFrequencyY;
      prim.numOctaves = turbulence->numOctaves;
      prim.seed = turbulence->seed;
      prim.stitchTiles = turbulence->stitchTiles;
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
    } else if (const auto* displace = registry.try_get<FEDisplacementMapComponent>(cur)) {
      filter_primitive::DisplacementMap prim;
      prim.scale = displace->scale;
      prim.xChannelSelector =
          static_cast<filter_primitive::DisplacementMap::Channel>(displace->xChannelSelector);
      prim.yChannelSelector =
          static_cast<filter_primitive::DisplacementMap::Channel>(displace->yChannelSelector);
      filterGraph.nodes.push_back(makeFilterNode2(prim, *primitive, primitiveCIF));
    } else if (const auto* diffuse = registry.try_get<FEDiffuseLightingComponent>(cur)) {
      filter_primitive::DiffuseLighting prim;
      prim.surfaceScale = diffuse->surfaceScale;
      prim.diffuseConstant = diffuse->diffuseConstant;
      prim.lightingColor =
          resolveLightingColor<FEDiffuseLightingComponent>(registry, cur, warningSink);

      // Find light source child element.
      const auto& curTree = registry.get<donner::components::TreeComponent>(cur);
      for (auto child = curTree.firstChild(); child != entt::null;
           child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
        const auto* lightComp = registry.try_get<LightSourceComponent>(child);
        if (lightComp) {
          filter_primitive::LightSource light;
          light.type = static_cast<filter_primitive::LightSource::Type>(lightComp->type);
          light.azimuth = lightComp->azimuth;
          light.elevation = lightComp->elevation;
          light.x = lightComp->x;
          light.y = lightComp->y;
          light.z = lightComp->z;
          light.pointsAtX = lightComp->pointsAtX;
          light.pointsAtY = lightComp->pointsAtY;
          light.pointsAtZ = lightComp->pointsAtZ;
          light.spotExponent = lightComp->spotExponent;
          light.limitingConeAngle = lightComp->limitingConeAngle;
          prim.light = light;
          break;  // Only first light source is used.
        }
      }

      filterGraph.nodes.push_back(makeFilterNode(std::move(prim), *primitive, primitiveCIF));
    } else if (const auto* specular = registry.try_get<FESpecularLightingComponent>(cur)) {
      filter_primitive::SpecularLighting prim;
      prim.surfaceScale = specular->surfaceScale;
      prim.specularConstant = specular->specularConstant;
      prim.specularExponent = specular->specularExponent;
      prim.lightingColor =
          resolveLightingColor<FESpecularLightingComponent>(registry, cur, warningSink);

      // Find light source child element.
      const auto& curTree = registry.get<donner::components::TreeComponent>(cur);
      for (auto child = curTree.firstChild(); child != entt::null;
           child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
        const auto* lightComp = registry.try_get<LightSourceComponent>(child);
        if (lightComp) {
          filter_primitive::LightSource light;
          light.type = static_cast<filter_primitive::LightSource::Type>(lightComp->type);
          light.azimuth = lightComp->azimuth;
          light.elevation = lightComp->elevation;
          light.x = lightComp->x;
          light.y = lightComp->y;
          light.z = lightComp->z;
          light.pointsAtX = lightComp->pointsAtX;
          light.pointsAtY = lightComp->pointsAtY;
          light.pointsAtZ = lightComp->pointsAtZ;
          light.spotExponent = lightComp->spotExponent;
          light.limitingConeAngle = lightComp->limitingConeAngle;
          prim.light = light;
          break;
        }
      }

      filterGraph.nodes.push_back(makeFilterNode(std::move(prim), *primitive, primitiveCIF));
    } else if (const auto* feImage = registry.try_get<FEImageComponent>(cur)) {
      AppendImageNode(registry, cur, filterGraph, *feImage, *primitive, primitiveCIF,
                      computedBudget);
    }
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
    createComputedFilter(EntityHandle(registry, entity), registry.get<FilterComponent>(entity),
                         warningSink);
  }
}

}  // namespace donner::svg::components
