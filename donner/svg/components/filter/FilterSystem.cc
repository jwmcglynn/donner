#include "donner/svg/components/filter/FilterSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

namespace {

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
FilterNode makeFilterNode(FilterPrimitive primitive, const FilterPrimitiveComponent& attrs) {
  FilterNode node;
  node.primitive = std::move(primitive);
  node.inputs.push_back(toFilterInput(attrs));
  node.result = attrs.result;
  node.x = attrs.x;
  node.y = attrs.y;
  node.width = attrs.width;
  node.height = attrs.height;
  return node;
}

/// Populate the common FilterNode fields for two-input primitives (in + in2).
FilterNode makeFilterNode2(FilterPrimitive primitive, const FilterPrimitiveComponent& attrs) {
  FilterNode node;
  node.primitive = std::move(primitive);
  node.inputs.push_back(toFilterInput(attrs));
  node.inputs.push_back(toFilterInput2(attrs));
  node.result = attrs.result;
  node.x = attrs.x;
  node.y = attrs.y;
  node.width = attrs.width;
  node.height = attrs.height;
  return node;
}

/// Resolve flood-color and flood-opacity from the FEFloodComponent properties overlaid with CSS
/// unparsed properties, and return a Flood primitive with the resolved values.
filter_primitive::Flood resolveFloodProperties(const Registry& registry, entt::entity cur,
                                               std::vector<ParseError>* outWarnings) {
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
            if (outWarnings) {
              outWarnings->emplace_back(std::move(maybeError.value()));
            }
          }
        } else if (name == "flood-opacity") {
          if (auto maybeError = Parse(
                  params,
                  [](const parser::PropertyParseFnParams& params) {
                    return parser::ParseAlphaValue(params.components());
                  },
                  &props.floodOpacity)) {
            if (outWarnings) {
              outWarnings->emplace_back(std::move(maybeError.value()));
            }
          }
        }
      }
    }

    // Resolve currentColor.
    if (props.floodColor.hasValue() && props.floodColor.getRequired().isCurrentColor()) {
      const auto& currentColor = style->properties->color;
      props.floodColor.set(currentColor.getRequired(), currentColor.specificity);
    }
  }

  if (props.floodColor.hasValue()) {
    flood.floodColor = props.floodColor.getRequired();
  }
  if (props.floodOpacity.hasValue()) {
    flood.floodOpacity = props.floodOpacity.getRequired();
  }

  return flood;
}

}  // namespace

void FilterSystem::createComputedFilter(EntityHandle handle, const FilterComponent& component,
                                        std::vector<ParseError>* outWarnings) {
  const Registry& registry = *handle.registry();

  std::vector<FilterEffect> effectChain;
  FilterGraph filterGraph;

  // Find all FilterPrimitiveComponent instances in this filter.
  const donner::components::TreeComponent& tree = handle.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<donner::components::TreeComponent>(cur).nextSibling()) {
    const auto* primitive = registry.try_get<FilterPrimitiveComponent>(cur);
    if (!primitive) {
      continue;
    }

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
          },
          *primitive));
    } else if (registry.try_get<FEFloodComponent>(cur)) {
      filterGraph.nodes.push_back(
          makeFilterNode(resolveFloodProperties(registry, cur, outWarnings), *primitive));
    } else if (const auto* offset = registry.try_get<FEOffsetComponent>(cur)) {
      filterGraph.nodes.push_back(makeFilterNode(
          filter_primitive::Offset{
              .dx = offset->dx,
              .dy = offset->dy,
          },
          *primitive));
    } else if (const auto* comp = registry.try_get<FECompositeComponent>(cur)) {
      filter_primitive::Composite prim;
      prim.op = static_cast<filter_primitive::Composite::Operator>(comp->op);
      prim.k1 = comp->k1;
      prim.k2 = comp->k2;
      prim.k3 = comp->k3;
      prim.k4 = comp->k4;
      filterGraph.nodes.push_back(makeFilterNode2(std::move(prim), *primitive));
    } else if (const auto* colorMatrix = registry.try_get<FEColorMatrixComponent>(cur)) {
      filter_primitive::ColorMatrix prim;
      prim.type = static_cast<filter_primitive::ColorMatrix::Type>(colorMatrix->type);
      prim.values = colorMatrix->values;
      filterGraph.nodes.push_back(makeFilterNode(std::move(prim), *primitive));
    } else if (const auto* blend = registry.try_get<FEBlendComponent>(cur)) {
      filter_primitive::Blend prim;
      prim.mode = static_cast<filter_primitive::Blend::Mode>(blend->mode);
      filterGraph.nodes.push_back(makeFilterNode2(std::move(prim), *primitive));
    } else if (registry.try_get<FEComponentTransferComponent>(cur)) {
      // feComponentTransfer: collect feFuncR/G/B/A children.
      filter_primitive::ComponentTransfer prim;

      const auto& curTree = registry.get<donner::components::TreeComponent>(cur);
      for (auto child = curTree.firstChild(); child != entt::null;
           child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
        const auto* func = registry.try_get<FEFuncComponent>(child);
        if (!func) {
          continue;
        }

        auto toFuncType = [](FEFuncComponent::FuncType t) {
          return static_cast<filter_primitive::ComponentTransfer::FuncType>(t);
        };

        filter_primitive::ComponentTransfer::Func f;
        f.type = toFuncType(func->type);
        f.tableValues = func->tableValues;
        f.slope = func->slope;
        f.intercept = func->intercept;
        f.amplitude = func->amplitude;
        f.exponent = func->exponent;
        f.offset = func->offset;

        switch (func->channel) {
          case FEFuncComponent::Channel::R: prim.funcR = std::move(f); break;
          case FEFuncComponent::Channel::G: prim.funcG = std::move(f); break;
          case FEFuncComponent::Channel::B: prim.funcB = std::move(f); break;
          case FEFuncComponent::Channel::A: prim.funcA = std::move(f); break;
        }
      }

      filterGraph.nodes.push_back(makeFilterNode(std::move(prim), *primitive));
    } else if (registry.try_get<FEMergeComponent>(cur)) {
      // feMerge: collect inputs from feMergeNode children.
      FilterNode node;
      node.primitive = filter_primitive::Merge{};
      node.result = primitive->result;
      node.x = primitive->x;
      node.y = primitive->y;
      node.width = primitive->width;
      node.height = primitive->height;

      // Walk feMergeNode children to collect inputs.
      const auto& curTree = registry.get<donner::components::TreeComponent>(cur);
      for (auto child = curTree.firstChild(); child != entt::null;
           child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
        const auto* mergeNode = registry.try_get<FEMergeNodeComponent>(child);
        if (mergeNode) {
          if (mergeNode->in.has_value()) {
            node.inputs.push_back(mergeNode->in.value());
          } else {
            node.inputs.push_back(FilterInput{FilterInput::Previous{}});
          }
        }
      }

      filterGraph.nodes.push_back(std::move(node));
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
                if (outWarnings) {
                  outWarnings->emplace_back(std::move(maybeError.value()));
                }
              }
            } else if (name == "flood-opacity") {
              if (auto maybeError = Parse(
                      params,
                      [](const parser::PropertyParseFnParams& params) {
                        return parser::ParseAlphaValue(params.components());
                      },
                      &props.floodOpacity)) {
                if (outWarnings) {
                  outWarnings->emplace_back(std::move(maybeError.value()));
                }
              }
            }
          }
        }

        // Resolve currentColor.
        if (props.floodColor.hasValue() && props.floodColor.getRequired().isCurrentColor()) {
          const auto& currentColor = style->properties->color;
          props.floodColor.set(currentColor.getRequired(), currentColor.specificity);
        }
      }

      if (props.floodColor.hasValue()) {
        prim.floodColor = props.floodColor.getRequired();
      }
      if (props.floodOpacity.hasValue()) {
        prim.floodOpacity = props.floodOpacity.getRequired();
      }

      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive));
    } else if (const auto* morph = registry.try_get<FEMorphologyComponent>(cur)) {
      filter_primitive::Morphology prim;
      prim.op = static_cast<filter_primitive::Morphology::Operator>(morph->op);
      prim.radiusX = morph->radiusX;
      prim.radiusY = morph->radiusY;
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive));
    } else if (const auto* convolve = registry.try_get<FEConvolveMatrixComponent>(cur)) {
      filter_primitive::ConvolveMatrix prim;
      prim.orderX = convolve->orderX;
      prim.orderY = convolve->orderY;
      prim.kernelMatrix = convolve->kernelMatrix;
      prim.divisor = convolve->divisor;
      prim.bias = convolve->bias;
      prim.targetX = convolve->targetX;
      prim.targetY = convolve->targetY;
      prim.edgeMode = static_cast<filter_primitive::ConvolveMatrix::EdgeMode>(convolve->edgeMode);
      prim.preserveAlpha = convolve->preserveAlpha;
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive));
    } else if (registry.try_get<FETileComponent>(cur)) {
      filterGraph.nodes.push_back(makeFilterNode(filter_primitive::Tile{}, *primitive));
    } else if (const auto* turbulence = registry.try_get<FETurbulenceComponent>(cur)) {
      filter_primitive::Turbulence prim;
      prim.type = static_cast<filter_primitive::Turbulence::Type>(turbulence->type);
      prim.baseFrequencyX = turbulence->baseFrequencyX;
      prim.baseFrequencyY = turbulence->baseFrequencyY;
      prim.numOctaves = turbulence->numOctaves;
      prim.seed = turbulence->seed;
      prim.stitchTiles = turbulence->stitchTiles;
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive));
    } else if (const auto* displace = registry.try_get<FEDisplacementMapComponent>(cur)) {
      filter_primitive::DisplacementMap prim;
      prim.scale = displace->scale;
      prim.xChannelSelector =
          static_cast<filter_primitive::DisplacementMap::Channel>(displace->xChannelSelector);
      prim.yChannelSelector =
          static_cast<filter_primitive::DisplacementMap::Channel>(displace->yChannelSelector);
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive));
    }
  }

  if (!effectChain.empty() || !filterGraph.empty()) {
    ComputedFilterComponent& computed = handle.emplace_or_replace<ComputedFilterComponent>();
    computed.effectChain = std::move(effectChain);
    computed.filterGraph = std::move(filterGraph);
    computed.x = component.x.value_or(computed.x);
    computed.y = component.y.value_or(computed.y);
    computed.width = component.width.value_or(computed.width);
    computed.height = component.height.value_or(computed.height);
    computed.filterUnits = component.filterUnits;
    computed.primitiveUnits = component.primitiveUnits;
    computed.colorInterpolationFilters = component.colorInterpolationFilters;
    computed.filterGraph.colorInterpolationFilters = component.colorInterpolationFilters;
  } else {
    handle.remove<ComputedFilterComponent>();
  }
}

void FilterSystem::instantiateAllComputedComponents(Registry& registry,
                                                    std::vector<ParseError>* outWarnings) {
  for (auto entity : registry.view<FilterComponent>()) {
    createComputedFilter(EntityHandle(registry, entity), registry.get<FilterComponent>(entity),
                         outWarnings);
  }
}

}  // namespace donner::svg::components
