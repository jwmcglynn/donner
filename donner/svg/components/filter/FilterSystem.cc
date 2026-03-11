#include "donner/svg/components/filter/FilterSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/graph/RecursionGuard.h"
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

/// Resolve lighting-color from a lighting component's Property, overlaid with CSS unparsed
/// properties, following the same pattern as resolveFloodProperties for flood-color.
template <typename LightingComponent>
css::Color resolveLightingColor(const Registry& registry, entt::entity cur,
                                std::vector<ParseError>* outWarnings) {
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
            if (outWarnings) {
              outWarnings->emplace_back(std::move(maybeError.value()));
            }
          }
        }
      }
    }

    // Resolve currentColor.
    if (lightingColor.hasValue() && lightingColor.getRequired().isCurrentColor()) {
      const auto& currentColor = style->properties->color;
      lightingColor.set(currentColor.getRequired(), currentColor.specificity);
    }
  }

  return lightingColor.getRequired();
}

/// Resolve color-interpolation-filters on an individual filter primitive entity.
/// Returns the per-primitive value if explicitly set, or std::nullopt if the filter-level
/// default should be used.
std::optional<ColorInterpolationFilters> resolveColorInterpolationFilters(const Registry& registry,
                                                                          entt::entity cur) {
  // Check CSS unparsed properties (from presentation attribute or style="").
  if (const auto* style = registry.try_get<ComputedStyleComponent>(cur)) {
    if (style->properties.has_value()) {
      for (const auto& [name, unparsedProperty] : style->properties->unparsedProperties) {
        if (name == "color-interpolation-filters") {
          const auto& decl = unparsedProperty.declaration;
          // Parse the simple keyword value.
          for (const auto& component : decl.values) {
            if (const auto* token = std::get_if<css::Token>(&component.value)) {
              if (token->is<css::Token::Ident>()) {
                const auto& ident = token->get<css::Token::Ident>().value;
                if (ident == "sRGB") {
                  return ColorInterpolationFilters::SRGB;
                }
                if (ident == "linearRGB") {
                  return ColorInterpolationFilters::LinearRGB;
                }
              }
            }
          }
        }
      }
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

std::vector<EntityHandle> getInheritanceChain(EntityHandle handle,
                                              std::vector<ParseError>* outWarnings) {
  Registry& registry = *handle.registry();

  std::vector<EntityHandle> inheritanceChain;
  inheritanceChain.push_back(handle);

  RecursionGuard guard;
  guard.add(handle);

  EntityHandle current = handle;
  while (const auto* filter = current.try_get<FilterComponent>()) {
    if (!filter->href.has_value()) {
      break;
    }

    auto resolvedReference = filter->href->resolve(registry);
    if (!resolvedReference.has_value()) {
      if (outWarnings) {
        ParseError err;
        err.reason = "Filter element href=\"" + filter->href->href + "\" failed to resolve";
        outWarnings->push_back(std::move(err));
      }
      break;
    }

    EntityHandle target = resolvedReference->handle;
    if (!target.valid() || !target.all_of<FilterComponent>()) {
      if (outWarnings) {
        ParseError err;
        err.reason =
            "Filter element href=\"" + filter->href->href + "\" does not reference a <filter>";
        outWarnings->push_back(std::move(err));
      }
      break;
    }

    if (guard.hasRecursion(target)) {
      if (outWarnings) {
        ParseError err;
        err.reason = "Circular filter inheritance detected";
        outWarnings->push_back(std::move(err));
      }
      break;
    }

    inheritanceChain.push_back(target);
    guard.add(target);
    current = target;
  }

  return inheritanceChain;
}

}  // namespace

void FilterSystem::createComputedFilter(EntityHandle handle, const FilterComponent& component,
                                        std::vector<ParseError>* outWarnings) {
  (void)component;

  const Registry& registry = *handle.registry();

  const std::vector<EntityHandle> inheritanceChain = getInheritanceChain(handle, outWarnings);

  EntityHandle primitiveSource;
  for (EntityHandle candidate : inheritanceChain) {
    if (hasFilterPrimitiveChildren(registry, candidate)) {
      primitiveSource = candidate;
      break;
    }
  }

  if (!primitiveSource.valid()) {
    handle.remove<ComputedFilterComponent>();
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
       cur = registry.get<donner::components::TreeComponent>(cur).nextSibling()) {
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
          *primitive));
    } else if (registry.try_get<FEFloodComponent>(cur)) {
      filterGraph.nodes.push_back(makeFilterNode(resolveFloodProperties(registry, cur, outWarnings),
                                                 *primitive, primitiveCIF));
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
      filterGraph.nodes.push_back(makeFilterNode2(std::move(prim), *primitive, primitiveCIF));
    } else if (const auto* colorMatrix = registry.try_get<FEColorMatrixComponent>(cur)) {
      filter_primitive::ColorMatrix prim;
      prim.type = static_cast<filter_primitive::ColorMatrix::Type>(colorMatrix->type);
      prim.values = colorMatrix->values;
      filterGraph.nodes.push_back(makeFilterNode(std::move(prim), *primitive, primitiveCIF));
    } else if (const auto* blend = registry.try_get<FEBlendComponent>(cur)) {
      filter_primitive::Blend prim;
      prim.mode = static_cast<filter_primitive::Blend::Mode>(blend->mode);
      filterGraph.nodes.push_back(makeFilterNode2(std::move(prim), *primitive, primitiveCIF));
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

      filterGraph.nodes.push_back(makeFilterNode(std::move(prim), *primitive, primitiveCIF));
    } else if (registry.try_get<FEMergeComponent>(cur)) {
      // feMerge: collect inputs from feMergeNode children.
      FilterNode node;
      node.primitive = filter_primitive::Merge{};
      node.result = primitive->result;
      node.x = primitive->x;
      node.y = primitive->y;
      node.width = primitive->width;
      node.height = primitive->height;
      node.colorInterpolationFilters = primitiveCIF;

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

      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
    } else if (const auto* morph = registry.try_get<FEMorphologyComponent>(cur)) {
      filter_primitive::Morphology prim;
      prim.op = static_cast<filter_primitive::Morphology::Operator>(morph->op);
      prim.radiusX = morph->radiusX;
      prim.radiusY = morph->radiusY;
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
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
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
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
      filterGraph.nodes.push_back(makeFilterNode(prim, *primitive, primitiveCIF));
    } else if (const auto* diffuse = registry.try_get<FEDiffuseLightingComponent>(cur)) {
      filter_primitive::DiffuseLighting prim;
      prim.surfaceScale = diffuse->surfaceScale;
      prim.diffuseConstant = diffuse->diffuseConstant;
      prim.lightingColor =
          resolveLightingColor<FEDiffuseLightingComponent>(registry, cur, outWarnings);

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
          resolveLightingColor<FESpecularLightingComponent>(registry, cur, outWarnings);

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
      filter_primitive::Image prim;
      prim.href = feImage->href;
      prim.preserveAspectRatio = feImage->preserveAspectRatio;

      // Load the referenced image if available.
      if (const auto* loaded = registry.try_get<LoadedImageComponent>(cur);
          loaded && loaded->image.has_value()) {
        prim.imageData = loaded->image->data;
        prim.imageWidth = loaded->image->width;
        prim.imageHeight = loaded->image->height;
      } else if (const auto* svgLoaded = registry.try_get<LoadedSVGImageComponent>(cur);
                 svgLoaded && svgLoaded->subDocument) {
        // SVG sub-document — store the pointer for the renderer to pre-render to pixels.
        prim.svgSubDocument = svgLoaded->subDocument;
      }

      // feImage has no standard input (it generates its own content).
      FilterNode node;
      node.primitive = std::move(prim);
      node.result = primitive->result;
      node.x = primitive->x;
      node.y = primitive->y;
      node.width = primitive->width;
      node.height = primitive->height;
      node.colorInterpolationFilters = primitiveCIF;
      filterGraph.nodes.push_back(std::move(node));
    }
  }

  if (!effectChain.empty() || !filterGraph.empty()) {
    ComputedFilterComponent& computed = handle.emplace_or_replace<ComputedFilterComponent>();
    computed.effectChain = std::move(effectChain);
    computed.filterGraph = std::move(filterGraph);
    computed.x = computedX;
    computed.y = computedY;
    computed.width = computedWidth;
    computed.height = computedHeight;
    computed.filterUnits = computedFilterUnits;
    computed.primitiveUnits = computedPrimitiveUnits;
    computed.colorInterpolationFilters = computedColorInterpolationFilters;
    computed.filterGraph.colorInterpolationFilters = computedColorInterpolationFilters;
    computed.filterGraph.primitiveUnits = computedPrimitiveUnits;
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
