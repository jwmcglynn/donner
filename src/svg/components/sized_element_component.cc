#include "src/svg/components/sized_element_component.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg::components {

namespace {

static constexpr int kDefaultWidth = 512;
static constexpr int kDefaultHeight = 512;

// The maximum size supported for a rendered image.
static constexpr int kMaxDimension = 8192;

using SizedElementPresentationAttributeParseFn = std::optional<ParseError> (*)(
    SizedElementProperties& properties, const PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, SizedElementPresentationAttributeParseFn, 4>
    kProperties = {
        {"x",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.x);
         }},  //
        {"y",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.y);
         }},  //
        {"width",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.width);
         }},  //
        {"height",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.height);
         }},
};

Vector2i RoundSize(Vector2f size) {
  return Vector2i(static_cast<int>(Round(size.x)), static_cast<int>(Round(size.y)));
}

PreserveAspectRatio GetPreserveAspectRatio(EntityHandle handle) {
  if (const auto* preserveAspectRatioComponent = handle.try_get<PreserveAspectRatioComponent>()) {
    return preserveAspectRatioComponent->preserveAspectRatio;
  }

  return PreserveAspectRatio::None();
}

ParseResult<bool> ParseSizedElementPresentationAttribute(EntityHandle handle, std::string_view name,
                                                         const PropertyParseFnParams& params) {
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

Boxd CalculateBounds(EntityHandle handle, const SizedElementProperties& properties,
                     const Boxd& inheritedViewbox, FontMetrics fontMetrics) {
  Registry& registry = *handle.registry();

  Vector2d size = inheritedViewbox.size();
  if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
    if (!properties.width.hasValue() && !properties.height.hasValue() && viewbox->viewbox) {
      size = viewbox->viewbox->size();
    }

    const DocumentContext& ctx = registry.ctx().get<DocumentContext>();
    if (ctx.rootEntity == handle.entity()) {
      // This is the root <svg> element.
      const Vector2i documentSize =
          handle.get<SizedElementComponent>().calculateViewportScaledDocumentSize(
              registry, InvalidSizeBehavior::ZeroSize);
      return Boxd(Vector2d(), documentSize);
    }
  }

  const ComputedShadowTreeComponent* shadowTree = handle.try_get<ComputedShadowTreeComponent>();

  // From https://www.w3.org/TR/SVG/struct.html#UseElement:
  // > The width and height attributes only have an effect if the referenced element defines a
  // > viewport (i.e., if it is a ‘svg’ or ‘symbol’)
  if (!shadowTree || (shadowTree && shadowTree->mainLightRoot() != entt::null &&
                      handle.registry()->all_of<ViewboxComponent>(shadowTree->mainLightRoot()))) {
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
      properties.x.getRequired().toPixels(Boxd(Vector2d(), size), fontMetrics, Lengthd::Extent::X),
      properties.y.getRequired().toPixels(Boxd(Vector2d(), size), fontMetrics, Lengthd::Extent::Y));
  return Boxd(origin, origin + size);
}

void ApplyUnparsedProperties(SizedElementProperties& properties,
                             const std::map<RcString, UnparsedProperty>& unparsedProperties,
                             std::vector<ParseError>* outWarnings) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError =
          it->second(properties, CreateParseFnParams(property.declaration, property.specificity,
                                                     PropertyParseBehavior::AllowUserUnits));
      if (maybeError && outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
    }
  }
}

Boxd ApplyUnparsedPropertiesAndGetBounds(
    EntityHandle handle, SizedElementProperties& properties,
    const std::map<RcString, UnparsedProperty>& unparsedProperties, Boxd inheritedViewbox,
    FontMetrics fontMetrics, std::vector<ParseError>* outWarnings) {
  ApplyUnparsedProperties(properties, unparsedProperties, outWarnings);
  return CalculateBounds(handle, properties, inheritedViewbox, fontMetrics);
}

template <typename T, PropertyCascade kCascade>
double IsAbsolute(const Property<T, kCascade>& property) {
  return property.hasValue() && property.getRequired().isAbsoluteSize();
}

template <typename T, PropertyCascade kCascade>
double GetDefiniteSize(const Property<T, kCascade>& property) {
  assert(IsAbsolute(property) && "Property must be absolute to get definite size");

  // Since we know the size is absolute, we don't need to specify a real viewbox or FontMetrics.
  return property.getRequired().toPixels(Boxd::CreateEmpty(Vector2d()), FontMetrics());
}

}  // namespace

std::optional<float> SizedElementComponent::intrinsicAspectRatio(Registry& registry) const {
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
  if (const auto* viewbox = registry.try_get<ViewboxComponent>(
          entt::to_entity(registry.storage<SizedElementComponent>(), *this));
      viewbox && viewbox->viewbox) {
    // > 1. let viewbox be the viewbox defined by the ‘viewBox’ attribute on the ‘svg’ element
    // > 2. return viewbox.width / viewbox.height
    return viewbox->viewbox->size().x / viewbox->viewbox->size().y;
  }

  // > 4. return null
  return std::nullopt;
}

Vector2i SizedElementComponent::calculateDocumentSize(Registry& registry) const {
  return RoundSize(calculateRawDocumentSize(registry));
}

Vector2i SizedElementComponent::calculateViewportScaledDocumentSize(
    Registry& registry, InvalidSizeBehavior behavior) const {
  const Vector2d documentSize = calculateDocumentSize(registry);
  const DocumentContext& ctx = registry.ctx().get<DocumentContext>();

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
      maybeCanvasSize = Vector2i(Min<int>(documentSize.x, kMaxDimension),
                                 Min<int>(documentSize.y, kMaxDimension));
    }
  }

  Vector2d scale = Vector2d(maybeCanvasSize.value()) / documentSize;
  scale.x = scale.y = std::min(scale.x, scale.y);

  const Transformd transform = Transformd::Scale(scale);
  return RoundSize(transform.transformPosition(documentSize));
}

ComputedSizedElementComponent::ComputedSizedElementComponent(
    EntityHandle handle, SizedElementProperties properties,
    const std::map<RcString, UnparsedProperty>& unparsedProperties, Boxd inheritedViewbox,
    FontMetrics fontMetrics, std::vector<ParseError>* outWarnings)
    : bounds(ApplyUnparsedPropertiesAndGetBounds(handle, properties, unparsedProperties,
                                                 inheritedViewbox, fontMetrics, outWarnings)),
      inheritedViewbox(inheritedViewbox) {}

std::optional<Boxd> ComputedSizedElementComponent::clipRect(EntityHandle handle) const {
  if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
    return bounds;
  }

  return std::nullopt;
}

Transformd ComputedSizedElementComponent::computeTransform(EntityHandle handle) const {
  // If this entity also has a viewbox, this SizedElementComponent is used to define a viewport.
  if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
    return viewbox->computeTransform(bounds, GetPreserveAspectRatio(handle));
  } else {
    // This branch is hit for <use> elements.
    PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio::None();
    if (const auto* preserveAspectRatioComponent = handle.try_get<PreserveAspectRatioComponent>()) {
      preserveAspectRatio = preserveAspectRatioComponent->preserveAspectRatio;
    }

    Vector2d scale = bounds.size() / inheritedViewbox.size();

    if (preserveAspectRatio.align != PreserveAspectRatio::Align::None) {
      if (preserveAspectRatio.meetOrSlice == PreserveAspectRatio::MeetOrSlice::Meet) {
        scale.x = scale.y = std::min(scale.x, scale.y);
      } else {
        scale.x = scale.y = std::max(scale.x, scale.y);
      }
    }

    Vector2d translation = bounds.topLeft - (inheritedViewbox.topLeft * scale);
    const Vector2d alignMaxOffset = bounds.size() - inheritedViewbox.size() * scale;

    const Vector2d alignMultiplier(preserveAspectRatio.alignMultiplierX(),
                                   preserveAspectRatio.alignMultiplierY());
    return Transformd::Scale(scale) *
           Transformd::Translate(translation + alignMaxOffset * alignMultiplier);
  }
}

void SizedElementComponent::computeWithPrecomputedStyle(EntityHandle handle,
                                                        const ComputedStyleComponent& style,
                                                        FontMetrics fontMetrics,
                                                        std::vector<ParseError>* outWarnings) {
  handle.emplace_or_replace<ComputedSizedElementComponent>(
      handle, properties, style.properties().unparsedProperties, style.viewbox(), fontMetrics,
      outWarnings);
}

void SizedElementComponent::compute(EntityHandle handle) {
  ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
  style.computeProperties(handle);

  return computeWithPrecomputedStyle(handle, style, FontMetrics(), nullptr);
}

Vector2d SizedElementComponent::calculateRawDocumentSize(Registry& registry) const {
  const DocumentContext& ctx = registry.ctx().get<DocumentContext>();

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

  const PreserveAspectRatio preserveAspectRatio =
      GetPreserveAspectRatio(EntityHandle(registry, ctx.rootEntity));

  // > If the specified size is only a width or height (but not both) then the concrete object size
  // > is given that specified width or height.
  if (definiteWidth || definiteHeight) {
    // > The other dimension is calculated as follows:

    // > 1. If the object has a natural aspect ratio, the missing dimension of the concrete object
    // > size is calculated using that aspect ratio and the present dimension.
    if (const auto maybeAspectRatio = intrinsicAspectRatio(registry);
        maybeAspectRatio && preserveAspectRatio != PreserveAspectRatio::None()) {
      if (!definiteWidth) {
        const double height = GetDefiniteSize(properties.height);
        return Vector2d(height * maybeAspectRatio.value(), height);
      } else {
        const double width = GetDefiniteSize(properties.width);
        return Vector2d(width, width / maybeAspectRatio.value());
      }
    }

    // TODO: What are the objects "natural dimensions" for "2. Otherwise, if the missing dimension
    // > is present in the object’s natural dimensions"

    // > 3. Otherwise, the missing dimension of the concrete object size is taken from the default
    // > object size.
    // TODO: PreserveAspectRatio

    if (!definiteWidth) {
      return Vector2d(canvasMaxBounds.size().x, GetDefiniteSize(properties.height));
    } else {
      return Vector2d(GetDefiniteSize(properties.width), canvasMaxBounds.size().y);
    }
  }

  // > If the specified size has no constraints:
  // TODO: Skipping "1. If the object has a natural height or width, its size is resolved as if its
  // natural dimensions were given as the specified size."
  // > 2. Otherwise, its size is resolved as a contain constraint against the default object size.
  const ViewboxComponent& viewbox = registry.get<ViewboxComponent>(ctx.rootEntity);
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
      viewbox.computeTransform(Boxd(Vector2d(), canvasSize), preserveAspectRatio);

  return transform.transformPosition(viewboxSize);
}

}  // namespace donner::svg::components

namespace donner::svg {

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

}  // namespace donner::svg
