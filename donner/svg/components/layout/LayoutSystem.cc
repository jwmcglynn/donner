#include "donner/svg/components/layout/LayoutSystem.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/ViewboxComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

namespace {

static constexpr int kDefaultWidth = 512;
static constexpr int kDefaultHeight = 512;

// The maximum size supported for a rendered image.
static constexpr int kMaxDimension = 8192;

using SizedElementPresentationAttributeParseFn = std::optional<parser::ParseError> (*)(
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

  return PreserveAspectRatio::None();
}

void ApplyUnparsedProperties(SizedElementProperties& properties,
                             const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                             std::vector<parser::ParseError>* outWarnings) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError = it->second(
          properties, CreateParseFnParams(property.declaration, property.specificity,
                                          parser::PropertyParseBehavior::AllowUserUnits));
      if (maybeError && outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
    }
  }
}

Boxd ApplyUnparsedPropertiesAndGetBounds(
    EntityHandle handle, SizedElementProperties& properties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties, Boxd inheritedViewbox,
    FontMetrics fontMetrics, std::vector<parser::ParseError>* outWarnings) {
  ApplyUnparsedProperties(properties, unparsedProperties, outWarnings);
  return LayoutSystem().calculateBounds(handle, properties, inheritedViewbox, fontMetrics);
}

parser::ParseResult<bool> ParseSizedElementPresentationAttribute(
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

  // Since we know the size is absolute, we don't need to specify a real viewbox or FontMetrics.
  return property.getRequired().toPixels(Boxd::CreateEmpty(Vector2d()), FontMetrics());
}

}  // namespace

std::optional<float> LayoutSystem::intrinsicAspectRatio(EntityHandle entity) const {
  const SizedElementProperties& properties = entity.get<SizedElementComponent>().properties;

  // Calculate the intrinsic aspect ratio per
  // https://svgwg.org/svg2-draft/coords.html#SizingSVGInCSS.

  // > 1. If the width and height sizing properties on the ‘svg’ element are both absolute values:
  if (IsAbsolute(properties.width) && IsAbsolute(properties.height)) {
    // > 1. return width / height
    // Since we know the size is absolute, we don't need to specify a real viewbox or FontMetrics.
    return GetDefiniteSize(properties.width) / GetDefiniteSize(properties.height);
  }

  // TODO(svg views): Do not handle "2. If an SVG View is active", this feature is not supported.

  // > 3. If the ‘viewBox’ on the ‘svg’ element is correctly specified:
  if (const auto* viewbox = entity.try_get<ViewboxComponent>(); viewbox && viewbox->viewbox) {
    // > 1. let viewbox be the viewbox defined by the ‘viewBox’ attribute on the ‘svg’ element
    // > 2. return viewbox.width / viewbox.height
    return viewbox->viewbox->size().x / viewbox->viewbox->size().y;
  }

  // > 4. return null
  return std::nullopt;
}

Vector2i LayoutSystem::calculateDocumentSize(EntityHandle entity) const {
  return RoundSize(calculateRawDocumentSize(entity));
}

Boxd LayoutSystem::calculateBounds(EntityHandle entity, const SizedElementProperties& properties,
                                   const Boxd& inheritedViewbox, FontMetrics fontMetrics) {
  Registry& registry = *entity.registry();

  Vector2d size = inheritedViewbox.size();
  if (const auto* viewbox = entity.try_get<ViewboxComponent>()) {
    if (!properties.width.hasValue() && !properties.height.hasValue() && viewbox->viewbox) {
      size = viewbox->viewbox->size();
    }

    const DocumentContext& ctx = registry.ctx().get<DocumentContext>();
    if (ctx.rootEntity == entity.entity()) {
      // This is the root <svg> element.
      const Vector2i documentSize =
          calculateViewportScaledDocumentSize(entity, InvalidSizeBehavior::ZeroSize);
      return Boxd(Vector2d(), documentSize);
    }
  }

  const ComputedShadowTreeComponent* shadowTree = entity.try_get<ComputedShadowTreeComponent>();

  // From https://www.w3.org/TR/SVG/struct.html#UseElement:
  // > The width and height attributes only have an effect if the referenced element defines a
  // > viewport (i.e., if it is a ‘svg’ or ‘symbol’)
  if (!shadowTree || (shadowTree && shadowTree->mainLightRoot() != entt::null &&
                      entity.registry()->all_of<ViewboxComponent>(shadowTree->mainLightRoot()))) {
    if (properties.width.hasValue()) {
      size.x = properties.width.getRequired().toPixels(inheritedViewbox, fontMetrics,
                                                       Lengthd::Extent::X);
    }

    if (properties.height.hasValue()) {
      size.y = properties.height.getRequired().toPixels(inheritedViewbox, fontMetrics,
                                                        Lengthd::Extent::Y);
    }
  }

  const Vector2d origin(
      properties.x.getRequired().toPixels(inheritedViewbox, fontMetrics, Lengthd::Extent::X),
      properties.y.getRequired().toPixels(inheritedViewbox, fontMetrics, Lengthd::Extent::Y));
  return Boxd(origin, origin + size);
}

Vector2i LayoutSystem::calculateViewportScaledDocumentSize(EntityHandle entity,
                                                           InvalidSizeBehavior behavior) const {
  const Vector2d documentSize = calculateDocumentSize(entity);
  const DocumentContext& ctx = entity.registry()->ctx().get<DocumentContext>();

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

Transformd LayoutSystem::computeTransform(
    EntityHandle handle, const ComputedSizedElementComponent& computedSizedElement) const {
  const PreserveAspectRatio& preserveAspectRatio = GetPreserveAspectRatio(handle);

  // If this entity also has a viewbox, this SizedElementComponent is used to define a viewport.
  if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
    return preserveAspectRatio.computeTransform(computedSizedElement.bounds, viewbox->viewbox);
  } else {
    // This branch is hit for <use> elements.
    return preserveAspectRatio.computeTransform(computedSizedElement.bounds,
                                                computedSizedElement.inheritedViewbox);
  }
}

void LayoutSystem::instantiateAllComputedComponents(Registry& registry,
                                                    std::vector<parser::ParseError>* outWarnings) {
  for (auto view = registry.view<SizedElementComponent, ComputedStyleComponent>();
       auto entity : view) {
    auto [component, style] = view.get(entity);
    createComputedSizedElementComponentWithStyle(EntityHandle(registry, entity), style,
                                                 FontMetrics(), outWarnings);
  }
}

// Evaluates SizedElementProperties and returns the resulting bounds.
Boxd LayoutSystem::computeSizeProperties(
    EntityHandle entity, const SizedElementProperties& sizeProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties, const Boxd& viewbox,
    FontMetrics fontMetrics, std::vector<parser::ParseError>* outWarnings) {
  SizedElementProperties mutableSizeProperties = sizeProperties;
  return ApplyUnparsedPropertiesAndGetBounds(entity, mutableSizeProperties, unparsedProperties,
                                             viewbox, fontMetrics, outWarnings);
}

// Creates a ComputedSizedElementComponent for the linked entity, using precomputed style
// information.
const ComputedSizedElementComponent& LayoutSystem::createComputedSizedElementComponentWithStyle(
    EntityHandle entity, const ComputedStyleComponent& style, FontMetrics fontMetrics,
    std::vector<parser::ParseError>* outWarnings) {
  SizedElementComponent& sizedElement = entity.get<SizedElementComponent>();

  const Boxd bounds =
      computeSizeProperties(entity, sizedElement.properties, style.properties->unparsedProperties,
                            style.viewbox.value(), fontMetrics, outWarnings);
  return entity.emplace_or_replace<ComputedSizedElementComponent>(bounds, style.viewbox.value());
}

std::optional<Boxd> LayoutSystem::clipRect(EntityHandle handle) const {
  if (handle.all_of<ViewboxComponent>()) {
    return handle.get<ComputedSizedElementComponent>().bounds;
  }

  return std::nullopt;
}

Vector2d LayoutSystem::calculateRawDocumentSize(EntityHandle entity) const {
  const SizedElementProperties& properties = entity.get<SizedElementComponent>().properties;
  const DocumentContext& ctx = entity.registry()->ctx().get<DocumentContext>();

  const std::optional<Vector2i> maybeCanvasSize = ctx.canvasSize;
  const Boxd canvasMaxBounds = Boxd::WithSize(
      maybeCanvasSize.has_value() ? *maybeCanvasSize : Vector2i(kDefaultWidth, kDefaultHeight));

  const bool definiteWidth = IsAbsolute(properties.width);
  const bool definiteHeight = IsAbsolute(properties.height);

  // Determine the document size based on the CSS Default Sizing Algorithm:
  // https://www.w3.org/TR/css-images-3/#default-sizing-algorithm

  // > If the specified size is a definite width and height, the concrete object size is given
  // that > width and height.
  if (definiteWidth && definiteHeight) {
    return Vector2d(GetDefiniteSize(properties.width), GetDefiniteSize(properties.height));
  }

  const PreserveAspectRatio preserveAspectRatio =
      GetPreserveAspectRatio(EntityHandle(*entity.registry(), ctx.rootEntity));

  // > If the specified size is only a width or height (but not both) then the concrete object
  // size > is given that specified width or height.
  if (definiteWidth || definiteHeight) {
    // > The other dimension is calculated as follows:

    // > 1. If the object has a natural aspect ratio, the missing dimension of the concrete
    // object > size is calculated using that aspect ratio and the present dimension.
    if (const auto maybeAspectRatio = intrinsicAspectRatio(entity);
        maybeAspectRatio && preserveAspectRatio != PreserveAspectRatio::None()) {
      if (!definiteWidth) {
        const double height = GetDefiniteSize(properties.height);
        return Vector2d(height * maybeAspectRatio.value(), height);
      } else {
        const double width = GetDefiniteSize(properties.width);
        return Vector2d(width, width / maybeAspectRatio.value());
      }
    }

    // TODO: What are the objects "natural dimensions" for "2. Otherwise, if the missing
    // dimension > is present in the object’s natural dimensions"

    // > 3. Otherwise, the missing dimension of the concrete object size is taken from the
    // default > object size.
    // TODO: PreserveAspectRatio

    if (!definiteWidth) {
      return Vector2d(canvasMaxBounds.size().x, GetDefiniteSize(properties.height));
    } else {
      return Vector2d(GetDefiniteSize(properties.width), canvasMaxBounds.size().y);
    }
  }

  // > If the specified size has no constraints:
  // TODO: Skipping "1. If the object has a natural height or width, its size is resolved as if
  // its natural dimensions were given as the specified size." > 2. Otherwise, its size is
  // resolved as a contain constraint against the default object size.
  const ViewboxComponent& viewbox = entity.registry()->get<ViewboxComponent>(ctx.rootEntity);
  if (!viewbox.viewbox) {
    return maybeCanvasSize.value_or(Vector2i(kDefaultWidth, kDefaultHeight));
  }

  const Vector2d viewboxSize = viewbox.viewbox->size();

  // If there's no canvas size, there's no scaling to do, so we can directly return the rounded
  // viewbox.
  if (!maybeCanvasSize) {
    return viewboxSize;
  }

  const Vector2d canvasSize(maybeCanvasSize.value());

  // Scale the original viewbox to the canvas size.
  const Transformd transform =
      preserveAspectRatio.computeTransform(Boxd(Vector2d(), canvasSize), viewbox.viewbox);

  return transform.transformPosition(viewboxSize);
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

}  // namespace donner::svg::parser
