#include "donner/svg/components/layout/LayoutSystem.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/parser/CssTransformParser.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/parser/TransformParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

namespace {

static constexpr int kDefaultWidth = 512;
static constexpr int kDefaultHeight = 512;

// The maximum size supported for a rendered image.
static constexpr int kMaxDimension = 8192;

using SizedElementPresentationAttributeParseFn = std::optional<ParseError> (*)(
    SizedElementProperties& properties, const parser::PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, SizedElementPresentationAttributeParseFn, 4>
    kProperties = {
        {"x",
         [](SizedElementProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.x);
         }},  //
        {"y",
         [](SizedElementProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.y);
         }},  //
        {"width",
         [](SizedElementProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.width);
         }},  //
        {"height",
         [](SizedElementProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.height);
         }},
};

Vector2i RoundSize(Vector2f size) {
  return Vector2i(static_cast<int>(Round(size.x)), static_cast<int>(Round(size.y)));
}

PreserveAspectRatio GetPreserveAspectRatio(EntityHandle entity) {
  if (const auto* preserveAspectRatioComponent = entity.try_get<PreserveAspectRatioComponent>()) {
    return preserveAspectRatioComponent->preserveAspectRatio;
  }

  return PreserveAspectRatio::Default();
}

void ApplyUnparsedProperties(SizedElementProperties& properties,
                             const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                             std::vector<ParseError>* outWarnings) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError = it->second(properties, parser::PropertyParseFnParams::Create(
                                                   property.declaration, property.specificity,
                                                   parser::PropertyParseBehavior::AllowUserUnits));
      if (maybeError && outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
    }
  }
}

ParseResult<bool> ParseSizedElementPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    SizedElementProperties& properties = handle.get_or_emplace<SizedElementComponent>().properties;
    auto maybeError = it->second(properties, params);
    if (maybeError) {
      return std::move(maybeError).value();
    } else {
      // Property found and parsed successfully.
      return true;
    }
  }

  return false;
}

template <typename T, PropertyCascade kCascade>
bool IsAbsolute(const Property<T, kCascade>& property) {
  return property.hasValue() && property.getRequired().isAbsoluteSize();
}

template <typename T, PropertyCascade kCascade>
double GetDefiniteSize(const Property<T, kCascade>& property) {
  assert(IsAbsolute(property) && "Property must be absolute to get definite size");

  // Since we know the size is absolute, we don't need to specify a real viewBox or FontMetrics.
  return property.getRequired().toPixels(Boxd::CreateEmpty(Vector2d()), FontMetrics());
}

Boxd GetViewBoxInternal(Registry& registry, Entity rootEntity, std::optional<Boxd> parentViewBox,
                        Entity currentEntity) {
  if (const auto* viewBoxComponent = registry.try_get<ComputedViewBoxComponent>(currentEntity)) {
    return viewBoxComponent->viewBox;
  } else {
    if (const auto* newViewBox = registry.try_get<ViewBoxComponent>(currentEntity)) {
      if (newViewBox->viewBox) {
        return newViewBox->viewBox.value();
      } else if (currentEntity != rootEntity &&
                 registry.all_of<SizedElementComponent>(currentEntity)) {
        const EntityHandle handle(registry, currentEntity);
        const ComputedStyleComponent& computedStyle = StyleSystem().computeStyle(handle, nullptr);

        const ComputedSizedElementComponent& computedSizedElement =
            LayoutSystem().createComputedSizedElementComponentWithStyle(handle, computedStyle,
                                                                        FontMetrics(), nullptr);
        return computedSizedElement.bounds;
      }
    }

    if (parentViewBox) {
      return parentViewBox.value();
    } else {
      // No viewBox found, use the document size.
      const Vector2i documentSize = LayoutSystem().calculateCanvasScaledDocumentSize(
          registry, LayoutSystem::InvalidSizeBehavior::ZeroSize);
      return Boxd(Vector2d::Zero(), documentSize);
    }
  }
}

}  // namespace

std::optional<float> LayoutSystem::intrinsicAspectRatio(EntityHandle entity) const {
  const SizedElementProperties& properties = entity.get<SizedElementComponent>().properties;

  // Calculate the intrinsic aspect ratio per
  // https://svgwg.org/svg2-draft/coords.html#SizingSVGInCSS.

  // > 1. If the width and height sizing properties on the 'svg' element are both absolute values:
  if (IsAbsolute(properties.width) && IsAbsolute(properties.height)) {
    // > 1. return width / height
    // Since we know the size is absolute, we don't need to specify a real viewBox or FontMetrics.
    return GetDefiniteSize(properties.width) / GetDefiniteSize(properties.height);
  }

  // TODO(svg views): Do not handle "2. If an SVG View is active", this feature is not supported.

  // > 3. If the 'viewBox' on the 'svg' element is correctly specified:
  if (const auto* viewBox = entity.try_get<ViewBoxComponent>(); viewBox && viewBox->viewBox) {
    // > 1. let viewBox be the viewBox defined by the 'viewBox' attribute on the 'svg' element
    // > 2. return viewBox.width / viewBox.height
    return viewBox->viewBox->size().x / viewBox->viewBox->size().y;
  }

  // > 4. return null
  return std::nullopt;
}

Vector2i LayoutSystem::calculateDocumentSize(Registry& registry) const {
  return RoundSize(calculateRawDocumentSize(registry));
}

Boxd LayoutSystem::getViewBox(EntityHandle entity) {
  if (const auto* computedViewBox = entity.try_get<ComputedViewBoxComponent>()) {
    return computedViewBox->viewBox;
  }

  Registry& registry = *entity.registry();
  SmallVector<Entity, 8> parents;

  std::optional<Boxd> parentViewBox;

  // Traverse up through the parent list until we find the root or a previously computed viewBox.
  for (Entity parent = entity; parent != entt::null;
       parent = registry.get<donner::components::TreeComponent>(parent).parent()) {
    if (const auto* computedViewBox = registry.try_get<ComputedViewBoxComponent>(parent)) {
      parentViewBox = computedViewBox->viewBox;
      break;
    }

    parents.push_back(parent);
  }

  assert(!parents.empty());

  // Now the parents list has parents in order from nearest -> root
  // Iterate from the end of the list to the start and cascade the viewBox.
  const Entity rootEntity = registry.ctx().get<SVGDocumentContext>().rootEntity;

  while (!parents.empty()) {
    Entity currentEntity = parents[parents.size() - 1];
    parents.pop_back();

    const Boxd currentViewBox =
        GetViewBoxInternal(registry, rootEntity, parentViewBox, currentEntity);
    registry.emplace<ComputedViewBoxComponent>(currentEntity, currentViewBox);

    parentViewBox = currentViewBox;
  }

  return parentViewBox.value();
}

bool LayoutSystem::overridesViewBox(EntityHandle entity) const {
  if (const auto* viewBoxComponent = entity.try_get<ViewBoxComponent>()) {
    return viewBoxComponent->viewBox.has_value();
  }

  return false;
}

Vector2i LayoutSystem::calculateCanvasScaledDocumentSize(Registry& registry,
                                                         InvalidSizeBehavior behavior) const {
  const Vector2d documentSize = calculateDocumentSize(registry);
  const auto& ctx = registry.ctx().get<SVGDocumentContext>();

  std::optional<Vector2i> maybeCanvasSize = ctx.canvasSize;
  if (documentSize.x <= 0.0 || documentSize.y <= 0.0) {
    if (behavior == InvalidSizeBehavior::ReturnDefault) {
      return maybeCanvasSize.value_or(Vector2i(kDefaultWidth, kDefaultHeight));
    } else {
      return Vector2i();
    }
  }

  if (!maybeCanvasSize) {
    if (documentSize.x <= kMaxDimension && documentSize.y <= kMaxDimension) {
      return RoundSize(documentSize);
    } else {
      maybeCanvasSize = Vector2i(Min<int>(static_cast<int>(documentSize.x), kMaxDimension),
                                 Min<int>(static_cast<int>(documentSize.y), kMaxDimension));
    }
  }

  Vector2d scale = Vector2d(maybeCanvasSize.value()) / documentSize;
  scale.x = scale.y = std::min(scale.x, scale.y);

  const Transformd transform = Transformd::Scale(scale);
  return RoundSize(transform.transformPosition(documentSize));
}

Transformd LayoutSystem::getEntityFromParentTransform(EntityHandle entity) {
  const ComputedStyleComponent& style = components::StyleSystem().computeStyle(entity, nullptr);

  const ComputedLocalTransformComponent& computedTransform =
      createComputedLocalTransformComponentWithStyle(entity, style, FontMetrics(), nullptr);

  return computedTransform.entityFromParent;
}

Transformd LayoutSystem::getDocumentFromCanvasTransform(Registry& registry) {
  EntityHandle rootEntity(registry, registry.ctx().get<SVGDocumentContext>().rootEntity);
  if (rootEntity.all_of<SizedElementComponent>()) {
    const ComputedStyleComponent& computedStyle = StyleSystem().computeStyle(rootEntity, nullptr);

    const ComputedSizedElementComponent& computedSizedElement =
        createComputedSizedElementComponentWithStyle(rootEntity, computedStyle, FontMetrics(),
                                                     nullptr);
    return elementContentFromViewBoxTransform(rootEntity, computedSizedElement);
  } else {
    return Transformd();
  }
}

Transformd LayoutSystem::getEntityContentFromEntityTransform(EntityHandle entity) {
  // If a shadow tree has been instantiated, there may be a ComputedShadowSizedElementComponent,
  // used for <symbol> elements.
  if (UTILS_PREDICT_FALSE(entity.all_of<ShadowTreeRootComponent>())) {
    EntityHandle lightEntity(*entity.registry(), entity.get<ShadowEntityComponent>().lightEntity);

    if (const auto* computedShadowSizedElement =
            entity.try_get<ComputedShadowSizedElementComponent>()) {
      // If there is no viewBox, we cannot apply scaling, return identity
      if (!overridesViewBox(lightEntity)) {
        return Transformd();
      }

      const PreserveAspectRatio& preserveAspectRatio = GetPreserveAspectRatio(lightEntity);

      const Boxd viewBox = getViewBox(lightEntity);
      const Transformd elementContentFromViewBox =
          preserveAspectRatio.elementContentFromViewBoxTransform(computedShadowSizedElement->bounds,
                                                                 viewBox);

      if (const auto* symbolComponent = lightEntity.try_get<SymbolComponent>()) {
        const Transformd symbolContentFromElementContent =
            Transformd::Translate(-symbolComponent->refX, -symbolComponent->refY);

        return symbolContentFromElementContent * elementContentFromViewBox;
      } else {
        return elementContentFromViewBox;
      }
    } else {
      return getEntityContentFromEntityTransform(lightEntity);
    }
  } else if (entity.all_of<SizedElementComponent>() &&
             entity.registry()->ctx().get<SVGDocumentContext>().rootEntity != entity.entity()) {
    const SizedElementComponent& sizedElement = entity.get<SizedElementComponent>();
    if (sizedElement.applyTranslationForUseElement) {
      return Transformd::Translate(entity.get<ComputedSizedElementComponent>().bounds.topLeft);
    }

    const ComputedStyleComponent& computedStyle = StyleSystem().computeStyle(entity, nullptr);

    const ComputedSizedElementComponent& computedSizedElement =
        createComputedSizedElementComponentWithStyle(entity, computedStyle, FontMetrics(), nullptr);
    const Transformd viewBoxTransform =
        elementContentFromViewBoxTransform(entity, computedSizedElement);

    return viewBoxTransform;
  }

  return Transformd();
}

void LayoutSystem::setEntityFromParentTransform(EntityHandle entity,
                                                const Transformd& entityFromParent) {
  auto& component = entity.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(entityFromParent), css::Specificity::Override());

  invalidate(entity);
}

const ComputedAbsoluteTransformComponent& LayoutSystem::getAbsoluteTransformComponent(
    EntityHandle entity) {
  if (const auto* computedAbsoluteTransform =
          entity.try_get<ComputedAbsoluteTransformComponent>()) {
    return *computedAbsoluteTransform;
  }

  Registry& registry = *entity.registry();
  SmallVector<Entity, 8> parents;

  Transformd parentFromWorld;
  bool worldIsCanvas = true;

  // Traverse up through the parent list until we find the root or a previously computed viewBox.
  for (Entity parent = entity;
       parent != entt::null &&
       registry.any_of<components::TransformComponent, components::ShadowEntityComponent>(parent);
       parent = registry.get<donner::components::TreeComponent>(parent).parent()) {
    if (const auto* computedAbsoluteTransform =
            registry.try_get<ComputedAbsoluteTransformComponent>(parent)) {
      parentFromWorld = computedAbsoluteTransform->entityFromWorld;
      worldIsCanvas = computedAbsoluteTransform->worldIsCanvas;
      break;
    }

    Entity lightEntity = parent;
    while (const auto* shadowEntity = registry.try_get<ShadowEntityComponent>(lightEntity)) {
      lightEntity = shadowEntity->lightEntity;
    }

    if (const auto* renderingBehavior = registry.try_get<RenderingBehaviorComponent>(lightEntity);
        renderingBehavior && !renderingBehavior->inheritsParentTransform) {
      parentFromWorld = Transformd();
      worldIsCanvas = false;
      if (renderingBehavior->appliesSelfTransform) {
        parents.push_back(parent);
      }

      break;
    }

    parents.push_back(parent);
  }

  if (parents.empty()) {
    return entity.emplace<ComputedAbsoluteTransformComponent>(parentFromWorld, worldIsCanvas);
  }

  // Now the parents list has parents in order from nearest -> root
  // Iterate from the end of the list to the start and cascade the transform.

  while (!parents.empty()) {
    EntityHandle currentHandle(registry, parents[parents.size() - 1]);
    parents.pop_back();

    const Transformd entityFromWorld = getEntityContentFromEntityTransform(currentHandle) *
                                       getEntityFromParentTransform(currentHandle) *
                                       parentFromWorld;
    currentHandle.emplace<ComputedAbsoluteTransformComponent>(entityFromWorld, worldIsCanvas);

    parentFromWorld = entityFromWorld;
  }

  return entity.get<ComputedAbsoluteTransformComponent>();
}

Transformd LayoutSystem::getEntityFromWorldTransform(EntityHandle entity) {
  return getAbsoluteTransformComponent(entity).entityFromWorld;
}

void LayoutSystem::invalidate(EntityHandle entity) {
  entity.remove<components::ComputedLocalTransformComponent>();
  entity.remove<components::ComputedAbsoluteTransformComponent>();
  entity.remove<components::ComputedSizedElementComponent>();
  entity.remove<components::ComputedShadowSizedElementComponent>();
  entity.remove<components::ComputedViewBoxComponent>();
}

Transformd LayoutSystem::elementContentFromViewBoxTransform(
    EntityHandle entity, const ComputedSizedElementComponent& computedSizedElement) const {
  const PreserveAspectRatio& preserveAspectRatio = GetPreserveAspectRatio(entity);
  // If this entity also has a viewBox, it defines a viewport.
  if (const auto* viewBox = entity.try_get<ViewBoxComponent>()) {
    return preserveAspectRatio.elementContentFromViewBoxTransform(computedSizedElement.bounds,
                                                                  viewBox->viewBox);
  } else if (entity.all_of<ImageComponent>()) {
    // Images compute their transform based on the image's intrinsic size, not the viewBox.
    // TODO: This should be based on the image's intrinsic size, move this transform computation
    // here from RendererSkia.
    return Transformd();
  } else {
    // This branch is hit for <use> elements.
    return preserveAspectRatio.elementContentFromViewBoxTransform(
        computedSizedElement.bounds, computedSizedElement.inheritedViewBox);
  }
}

void LayoutSystem::instantiateAllComputedComponents(Registry& registry,
                                                    std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<SizedElementComponent, ComputedStyleComponent>();
       auto entity : view) {
    auto [component, style] = view.get(entity);
    createComputedSizedElementComponentWithStyle(EntityHandle(registry, entity), style,
                                                 FontMetrics(), outWarnings);
  }

  for (auto view = registry.view<TransformComponent, ComputedStyleComponent>();
       auto entity : view) {
    auto [transform, style] = view.get(entity);
    createComputedLocalTransformComponentWithStyle(EntityHandle(registry, entity), style,
                                                   FontMetrics(), outWarnings);
  }

  // Now traverse the tree from the root down and compute values that inherit from the parent.
  // TODO(jwmcglynn): Also calculate the absolute transform
  struct ElementContext {
    Entity entity;
    std::optional<Boxd> parentViewBox;
  };

  const Entity rootEntity = registry.ctx().get<SVGDocumentContext>().rootEntity;

  SmallVector<ElementContext, 16> stack;
  stack.push_back(ElementContext{rootEntity, std::nullopt});

  while (!stack.empty()) {
    ElementContext current = stack[stack.size() - 1];
    stack.pop_back();

    const Boxd currentViewBox =
        GetViewBoxInternal(registry, rootEntity, current.parentViewBox, current.entity);
    registry.emplace_or_replace<ComputedViewBoxComponent>(current.entity, currentViewBox);

    for (Entity child =
             registry.get<donner::components::TreeComponent>(current.entity).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      stack.push_back(ElementContext{child, currentViewBox});
    }
  }
}

// Evaluates SizedElementProperties and returns the resulting bounds.
Boxd LayoutSystem::computeSizeProperties(
    EntityHandle entity, const SizedElementProperties& sizeProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties, const Boxd& viewBox,
    FontMetrics fontMetrics, std::vector<ParseError>* outWarnings) {
  SizedElementProperties mutableSizeProperties = sizeProperties;

  ApplyUnparsedProperties(mutableSizeProperties, unparsedProperties, outWarnings);
  return calculateSizedElementBounds(entity, mutableSizeProperties, viewBox, fontMetrics);
}

// Creates a ComputedSizedElementComponent for the linked entity, using precomputed style
// information.
const ComputedSizedElementComponent& LayoutSystem::createComputedSizedElementComponentWithStyle(
    EntityHandle entity, const ComputedStyleComponent& style, FontMetrics fontMetrics,
    std::vector<ParseError>* outWarnings) {
  SizedElementComponent& sizedElement = entity.get<SizedElementComponent>();

  const Entity parent = entity.get<donner::components::TreeComponent>().parent();
  const Boxd viewBox = parent != entt::null ? getViewBox(EntityHandle(*entity.registry(), parent))
                                            : getViewBox(entity);

  const Boxd bounds =
      computeSizeProperties(entity, sizedElement.properties, style.properties->unparsedProperties,
                            viewBox, fontMetrics, outWarnings);
  return entity.emplace_or_replace<ComputedSizedElementComponent>(bounds, viewBox);
}

const ComputedLocalTransformComponent& LayoutSystem::createComputedLocalTransformComponentWithStyle(
    EntityHandle handle, const ComputedStyleComponent& style, const FontMetrics& fontMetrics,
    std::vector<ParseError>* outWarnings) {
  std::optional<TransformComponent> shadowTransform;
  EntityHandle lightEntity = handle;
  if (const auto* shadowComponent = lightEntity.try_get<ShadowEntityComponent>()) {
    lightEntity = EntityHandle(*handle.registry(), shadowComponent->lightEntity);
  }

  TransformComponent& transform = lightEntity.get<TransformComponent>();

  // TODO(jwmcglynn): This should avoid recomputing the transform each request.
  const auto& properties = style.properties->unparsedProperties;
  if (auto it = properties.find("transform"); it != properties.end()) {
    const parser::UnparsedProperty& property = it->second;

    parser::PropertyParseFnParams params;
    params.valueOrComponents = property.declaration.values;
    params.specificity = property.specificity;
    params.parseBehavior = parser::PropertyParseBehavior::AllowUserUnits;

    auto maybeError = Parse(
        params,
        [](const parser::PropertyParseFnParams& params) {
          if (const std::string_view* str =
                  std::get_if<std::string_view>(&params.valueOrComponents)) {
            return parser::TransformParser::Parse(*str).map<CssTransform>(
                [](const Transformd& transform) { return CssTransform(transform); });
          } else {
            return parser::CssTransformParser::Parse(params.components());
          }
        },
        &transform.transform);

    if (maybeError && outWarnings) {
      outWarnings->emplace_back(std::move(maybeError.value()));
    }
  }

  auto& computedTransform = handle.get_or_emplace<ComputedLocalTransformComponent>();
  if (transform.transform.get()) {
    computedTransform.rawCssTransform = transform.transform.get().value();
    computedTransform.entityFromParent =
        transform.transform.get().value().compute(getViewBox(handle), fontMetrics);
  } else {
    computedTransform.entityFromParent = Transformd();
  }

  return computedTransform;
}

std::optional<Boxd> LayoutSystem::clipRect(EntityHandle handle) const {
  // Check for shadow sized element component
  if (const auto* shadowSizedElement = handle.try_get<ComputedShadowSizedElementComponent>()) {
    return shadowSizedElement->bounds;
  }

  // Check for regular sized element component
  if (handle.all_of<ViewBoxComponent>()) {
    if (const auto* sizedElement = handle.try_get<ComputedSizedElementComponent>()) {
      return sizedElement->bounds;
    }
  }

  return std::nullopt;
}

Boxd LayoutSystem::calculateSizedElementBounds(EntityHandle entity,
                                               const SizedElementProperties& properties,
                                               const Boxd& inheritedViewBox,
                                               FontMetrics fontMetrics) {
  Registry& registry = *entity.registry();

  Vector2d size = inheritedViewBox.size();
  if (const auto* viewBox = entity.try_get<ViewBoxComponent>()) {
    if (!properties.width.hasValue() && !properties.height.hasValue() && viewBox->viewBox) {
      size = viewBox->viewBox->size();
    }

    const auto& ctx = registry.ctx().get<SVGDocumentContext>();
    if (ctx.rootEntity == entity.entity()) {
      // This is the root <svg> element.
      const Vector2i documentSize =
          calculateCanvasScaledDocumentSize(registry, InvalidSizeBehavior::ZeroSize);
      return Boxd(Vector2d(), documentSize);
    }
  }

  const ComputedShadowTreeComponent* shadowTree = entity.try_get<ComputedShadowTreeComponent>();

  // From https://www.w3.org/TR/SVG/struct.html#UseElement:
  // > The width and height attributes only have an effect if the referenced element defines a
  // > viewport (i.e., if it is a 'svg' or 'symbol')
  if (!shadowTree || (shadowTree && shadowTree->mainLightRoot() != entt::null &&
                      entity.registry()->all_of<ViewBoxComponent>(shadowTree->mainLightRoot()))) {
    if (properties.width.hasValue()) {
      size.x = properties.width.getRequired().toPixels(inheritedViewBox, fontMetrics,
                                                       Lengthd::Extent::X);
    }

    if (properties.height.hasValue()) {
      size.y = properties.height.getRequired().toPixels(inheritedViewBox, fontMetrics,
                                                        Lengthd::Extent::Y);
    }
  }

  const Vector2d origin(
      properties.x.getRequired().toPixels(inheritedViewBox, fontMetrics, Lengthd::Extent::X),
      properties.y.getRequired().toPixels(inheritedViewBox, fontMetrics, Lengthd::Extent::Y));

  if (registry.all_of<ImageComponent>(entity)) {
    if (auto maybeImageSize = registry.ctx().get<ResourceManagerContext>().getImageSize(entity)) {
      const Vector2i imageSize = *maybeImageSize;

      // Use the default sizing algorithm to detect the size if any parameters are missing.
      // See https://www.w3.org/TR/css-images-3/#default-sizing
      if (properties.width.hasValue() && properties.height.hasValue()) {
        return Boxd(origin, origin + size);
      } else if (!properties.width.hasValue() && !properties.height.hasValue()) {
        size = Vector2d(imageSize);
      } else {
        const float aspectRatio = static_cast<float>(imageSize.x) / static_cast<float>(imageSize.y);

        if (!properties.width.hasValue()) {
          size.x = properties.height.getRequired().toPixels(inheritedViewBox, fontMetrics,
                                                            Lengthd::Extent::X) *
                   aspectRatio;
        } else if (!properties.height.hasValue()) {
          size.y = properties.width.getRequired().toPixels(inheritedViewBox, fontMetrics,
                                                           Lengthd::Extent::Y) /
                   aspectRatio;
        }
      }
    }
  } else if (registry.all_of<MaskComponent>(entity)) {
    // The bounds of a shadow entity are determined by the light entity.
    return Boxd(origin, origin + size);
  }

  return Boxd(origin, origin + size);
}

Vector2d LayoutSystem::calculateRawDocumentSize(Registry& registry) const {
  const auto& ctx = registry.ctx().get<SVGDocumentContext>();
  const EntityHandle root(registry, ctx.rootEntity);
  const SizedElementProperties& properties = root.get<SizedElementComponent>().properties;

  const std::optional<Vector2i> maybeCanvasSize = ctx.canvasSize;
  const Boxd canvasMaxBounds = Boxd::WithSize(
      maybeCanvasSize.has_value() ? *maybeCanvasSize : Vector2i(kDefaultWidth, kDefaultHeight));

  const bool definiteWidth = IsAbsolute(properties.width);
  const bool definiteHeight = IsAbsolute(properties.height);

  // Determine the document size based on the CSS Default Sizing Algorithm:
  // https://www.w3.org/TR/css-images-3/#default-sizing-algorithm

  // > If the specified size is a definite width and height, the concrete object size is given that
  // > width and height.
  if (definiteWidth && definiteHeight) {
    return Vector2d(GetDefiniteSize(properties.width), GetDefiniteSize(properties.height));
  }

  const PreserveAspectRatio preserveAspectRatio = GetPreserveAspectRatio(root);

  // > If the specified size is only a width or height (but not both) then the concrete object size
  // > is given that specified width or height.
  if (definiteWidth || definiteHeight) {
    // > The other dimension is calculated as follows:

    // > 1. If the object has a natural aspect ratio, the missing dimension of the concrete object
    // > size is calculated using that aspect ratio and the present dimension.
    if (const auto maybeAspectRatio = intrinsicAspectRatio(root);
        maybeAspectRatio && preserveAspectRatio != PreserveAspectRatio::None()) {
      if (!definiteWidth) {
        const double height = GetDefiniteSize(properties.height);
        return Vector2d(height * maybeAspectRatio.value(), height);
      } else {
        const double width = GetDefiniteSize(properties.width);
        return Vector2d(width, width / maybeAspectRatio.value());
      }
    }

    // TODO(jwmcglynn): What are the objects "natural dimensions" for "2. Otherwise, if the missing
    // dimension is present in the object's natural dimensions"

    // > 3. Otherwise, the missing dimension of the concrete object size is taken from the default
    // > object size.
    // TODO(jwmcglynn): PreserveAspectRatio

    if (!definiteWidth) {
      return Vector2d(canvasMaxBounds.size().x, GetDefiniteSize(properties.height));
    } else {
      return Vector2d(GetDefiniteSize(properties.width), canvasMaxBounds.size().y);
    }
  }

  // > If the specified size has no constraints:
  // TODO(jwmcglynn): Skipping "1. If the object has a natural height or width, its size is resolved
  // as if its natural dimensions were given as the specified size."
  //
  // > 2. Otherwise, its size is resolved as a contain constraint against the default object size.
  const ViewBoxComponent& viewBox = root.get<ViewBoxComponent>();
  if (!viewBox.viewBox) {
    return maybeCanvasSize.value_or(Vector2i(kDefaultWidth, kDefaultHeight));
  }

  const Vector2d viewBoxSize = viewBox.viewBox->size();

  // If there's no canvas size, there's no scaling to do, so we can directly return the rounded
  // viewBox.
  if (!maybeCanvasSize) {
    return viewBoxSize;
  }

  const Vector2d canvasSize(maybeCanvasSize.value());

  // Scale the original viewBox to the canvas size.
  const Transformd transform = preserveAspectRatio.elementContentFromViewBoxTransform(
      Boxd(Vector2d(), canvasSize), viewBox.viewBox);

  return transform.transformPosition(viewBoxSize);
}

bool LayoutSystem::createShadowSizedElementComponent(Registry& registry, Entity shadowEntity,
                                                     EntityHandle useEntity, Entity symbolEntity,
                                                     ShadowBranchType branchType,
                                                     std::vector<ParseError>* outWarnings) {
  // TODO: Plumb FontMetrics
  FontMetrics fontMetrics;

  if (branchType != ShadowBranchType::Main) {
    return false;
  }

  // Must be sized elements
  const auto* parentSizedElement = useEntity.try_get<SizedElementComponent>();
  const auto* targetSizedElement = registry.try_get<SizedElementComponent>(symbolEntity);
  if (!parentSizedElement || !targetSizedElement ||
      !targetSizedElement->canOverrideWidthHeightForSymbol) {
    return false;
  }

  const Boxd parentViewBox = getViewBox(useEntity);

  // Override the width/height if the parent element specifies them
  SizedElementProperties properties = targetSizedElement->properties;

  if (parentSizedElement->properties.width.hasValue()) {
    properties.width = parentSizedElement->properties.width;
  }
  if (parentSizedElement->properties.height.hasValue()) {
    properties.height = parentSizedElement->properties.height;
  }

  Vector2d size = parentViewBox.size();

  if (properties.width.hasValue()) {
    size.x =
        properties.width.getRequired().toPixels(parentViewBox, fontMetrics, Lengthd::Extent::X);
  }
  if (properties.height.hasValue()) {
    size.y =
        properties.height.getRequired().toPixels(parentViewBox, fontMetrics, Lengthd::Extent::Y);
  }

  const Vector2d origin(
      properties.x.getRequired().toPixels(parentViewBox, fontMetrics, Lengthd::Extent::X),
      properties.y.getRequired().toPixels(parentViewBox, fontMetrics, Lengthd::Extent::Y));

  // Create the shadow component
  auto& shadowSized =
      registry.emplace_or_replace<ComputedShadowSizedElementComponent>(shadowEntity);
  shadowSized.bounds = Boxd(origin, origin + size);

  return true;
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

// SVGSVGElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::SVG>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return components::ParseSizedElementPresentationAttribute(handle, name, params);
}

// SVGUseElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Use>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return components::ParseSizedElementPresentationAttribute(handle, name, params);
}

// SVGImageElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Image>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return components::ParseSizedElementPresentationAttribute(handle, name, params);
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Symbol>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <symbol> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg::parser
