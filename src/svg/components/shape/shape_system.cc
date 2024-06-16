#include "src/svg/components/shape/shape_system.h"

#include <concepts>

#include "src/svg/components/shape/computed_path_component.h"
#include "src/svg/components/shape/rect_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/style/style_system.h"
#include "src/svg/parser/path_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

namespace {

ParseResult<RcString> ParseD(std::span<const css::ComponentValue> components) {
  if (auto maybeIdent = TryGetSingleIdent(components);
      maybeIdent && maybeIdent->equalsLowercase("none")) {
    return RcString();
  } else if (components.size() == 1) {
    if (const auto* str = components[0].tryGetToken<css::Token::String>()) {
      return str->value;
    }
  }

  ParseError err;
  err.reason = "Expected string or 'none'";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
}

std::optional<ParseError> ParseDFromAttributes(PathComponent& properties,
                                               const PropertyParseFnParams& params) {
  if (const std::string_view* str = std::get_if<std::string_view>(&params.valueOrComponents)) {
    properties.d.set(RcString(*str), params.specificity);
  } else {
    auto maybeError = Parse(
        params, [](const PropertyParseFnParams& params) { return ParseD(params.components()); },
        &properties.d);
    if (maybeError) {
      return std::move(maybeError.value());
    }
  }

  return std::nullopt;
}

template <typename F, typename ComponentType>
concept ForEachCallback = requires(const F& f) {
  { f.template operator()<ComponentType>() } -> std::same_as<bool>;
};

/**
 *
 * Helper to call the callback on each entt::type_list element. Short-circuits if the callback
returns true.
 *
 * @tparam TypeList
 * @tparam F
 * @tparam Indices
 * @param f
 * @return true
 * @return false
 */
template <typename TypeList, typename F, std::size_t... Indices>
  requires(ForEachCallback<F, typename entt::type_list_element<Indices, TypeList>::type> && ...)
constexpr bool ForEachShapeImpl(const F& f, std::index_sequence<Indices...>) {
  return (f.template operator()<typename entt::type_list_element<Indices, TypeList>::type>() ||
          ...);
}

// Main function to iterate over the tuple
template <typename TypeList, typename F>
constexpr bool ForEachShape(const F& f) {
  return ForEachShapeImpl<TypeList>(f, std::make_index_sequence<TypeList::size>{});
}

}  // namespace

const ComputedPathComponent* ShapeSystem::createComputedPathIfShape(
    EntityHandle handle, const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedPathComponent* computedPath = nullptr;

  ForEachShape<AllShapes>([&]<typename ShapeType>() -> bool {
    using ShapeComponent = ShapeType;
    bool shouldExit = false;

    if (handle.all_of<ShapeComponent>()) {
      const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, outWarnings);
      computedPath = createComputedShapeWithStyle(handle, handle.get<ShapeComponent>(), style,
                                                  fontMetrics, outWarnings);
      // Note the computedPath may be null if the shape failed to instantiate due to an error (like
      // having zero points), when this occurs no other shapes will match and we should exit early.
      shouldExit = true;
    }

    return shouldExit;
  });

  return computedPath;
}

void ShapeSystem::instantiateAllComputedPaths(Registry& registry,
                                              std::vector<ParseError>* outWarnings) {
  ForEachShape<AllShapes>([&]<typename ShapeType>() {
    for (auto view = registry.view<ShapeType, ComputedStyleComponent>(); auto entity : view) {
      auto [shape, style] = view.get(entity);
      createComputedShapeWithStyle(EntityHandle(registry, entity), shape, style, FontMetrics(),
                                   outWarnings);
    }

    const bool shouldExit = false;
    return shouldExit;
  });
}

const ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const CircleComponent& circle, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedCircleComponent& computedCircle = handle.get_or_emplace<ComputedCircleComponent>(
      circle.properties, style.properties->unparsedProperties, outWarnings);

  const Vector2d center(computedCircle.properties.cx.getRequired().toPixels(
                            style.viewbox.value(), fontMetrics, Lengthd::Extent::X),
                        computedCircle.properties.cy.getRequired().toPixels(
                            style.viewbox.value(), fontMetrics, Lengthd::Extent::Y));
  const double radius =
      computedCircle.properties.r.getRequired().toPixels(style.viewbox.value(), fontMetrics);

  if (radius > 0.0) {
    return &handle.emplace_or_replace<ComputedPathComponent>(
        PathSpline::Builder().circle(center, radius).build());
  } else {
    return nullptr;
  }
}

const ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const EllipseComponent& ellipse, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedEllipseComponent& computedEllipse = handle.get_or_emplace<ComputedEllipseComponent>(
      ellipse.properties, style.properties->unparsedProperties, outWarnings);

  const Vector2d center(
      computedEllipse.properties.cx.getRequired().toPixels(style.viewbox.value(), fontMetrics),
      computedEllipse.properties.cy.getRequired().toPixels(style.viewbox.value(), fontMetrics));
  const Vector2d radius(
      std::get<1>(computedEllipse.properties.calculateRx(style.viewbox.value(), fontMetrics)),
      std::get<1>(computedEllipse.properties.calculateRy(style.viewbox.value(), fontMetrics)));

  if (radius.x > 0.0 && radius.y > 0.0) {
    return &handle.emplace_or_replace<ComputedPathComponent>(
        PathSpline::Builder().ellipse(center, radius).build());
  } else {
    return nullptr;
  }
}

const ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const LineComponent& line, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const Vector2d start(line.x1.toPixels(style.viewbox.value(), fontMetrics),
                       line.y1.toPixels(style.viewbox.value(), fontMetrics));
  const Vector2d end(line.x2.toPixels(style.viewbox.value(), fontMetrics),
                     line.y2.toPixels(style.viewbox.value(), fontMetrics));

  return &handle.emplace_or_replace<ComputedPathComponent>(
      PathSpline::Builder().moveTo(start).lineTo(end).build());
}

const ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const PathComponent& path, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  Property<RcString> actualD = path.d;
  const auto& properties = style.properties->unparsedProperties;
  if (auto it = properties.find("d"); it != properties.end()) {
    auto maybeError = Parse(
        CreateParseFnParams(it->second.declaration, it->second.specificity,
                            PropertyParseBehavior::AllowUserUnits),
        [](const PropertyParseFnParams& params) { return ParseD(params.components()); }, &actualD);
    if (maybeError) {
      if (outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
      return nullptr;
    }
  }

  if (actualD.hasValue()) {
    auto maybePath = PathParser::Parse(actualD.get().value());
    if (maybePath.hasError()) {
      // Propagate warnings, which may be set on success too.
      if (outWarnings) {
        outWarnings->emplace_back(std::move(maybePath.error()));
      }
    }

    if (maybePath.hasResult() && !maybePath.result().empty()) {
      // Success: Return path
      return &handle.emplace_or_replace<ComputedPathComponent>(std::move(maybePath.result()));
    }
  }

  // Failed: Could not parse path
  handle.remove<ComputedPathComponent>();
  return nullptr;
}

const ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const PolyComponent& poly, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  PathSpline::Builder builder;
  if (!poly.points.empty()) {
    builder.moveTo(poly.points[0]);
  }

  for (size_t i = 1; i < poly.points.size(); ++i) {
    builder.lineTo(poly.points[i]);
  }

  if (poly.type == PolyComponent::Type::Polygon) {
    builder.closePath();
  }

  return &handle.emplace_or_replace<ComputedPathComponent>(builder.build());
}

const ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const RectComponent& rect, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedRectComponent& computedRect = handle.get_or_emplace<ComputedRectComponent>(
      rect.properties, style.properties->unparsedProperties, outWarnings);

  const Vector2d pos(computedRect.properties.x.getRequired().toPixels(
                         style.viewbox.value(), fontMetrics, Lengthd::Extent::X),
                     computedRect.properties.y.getRequired().toPixels(
                         style.viewbox.value(), fontMetrics, Lengthd::Extent::Y));
  const Vector2d size(computedRect.properties.width.getRequired().toPixels(
                          style.viewbox.value(), fontMetrics, Lengthd::Extent::X),
                      computedRect.properties.height.getRequired().toPixels(
                          style.viewbox.value(), fontMetrics, Lengthd::Extent::Y));

  if (size.x > 0.0 && size.y > 0.0) {
    if (computedRect.properties.rx.hasValue() || computedRect.properties.ry.hasValue()) {
      // 4/3 * (1 - cos(45 deg) / sin(45 deg) = 4/3 * (sqrt 2) - 1
      const double arcMagic = 0.5522847498;
      const Vector2d radius(
          Clamp(
              std::get<1>(computedRect.properties.calculateRx(style.viewbox.value(), fontMetrics)),
              0.0, size.x * 0.5),
          Clamp(
              std::get<1>(computedRect.properties.calculateRy(style.viewbox.value(), fontMetrics)),
              0.0, size.y * 0.5));

      // Success: Draw a rect with rounded corners.
      return &handle.emplace_or_replace<ComputedPathComponent>(
          PathSpline::Builder()
              // Draw top line.
              .moveTo(pos + Vector2d(radius.x, 0))
              .lineTo(pos + Vector2d(size.x - radius.x, 0.0))
              // Curve to the right line.
              .curveTo(pos + Vector2d(size.x - radius.x + radius.x * arcMagic, 0.0),
                       pos + Vector2d(size.x, radius.y - radius.y * arcMagic),
                       pos + Vector2d(size.x, radius.y))
              // Draw right line.
              .lineTo(pos + Vector2d(size.x, size.y - radius.y))
              // Curve to the bottom line.
              .curveTo(pos + Vector2d(size.x, size.y - radius.y + radius.y * arcMagic),
                       pos + Vector2d(size.x - radius.x + radius.x * arcMagic, size.y),
                       pos + Vector2d(size.x - radius.x, size.y))
              // Draw bottom line.
              .lineTo(pos + Vector2d(radius.x, size.y))
              // Curve to the left line.
              .curveTo(pos + Vector2d(radius.x - radius.x * arcMagic, size.y),
                       pos + Vector2d(0.0, size.y - radius.y + radius.y * arcMagic),
                       pos + Vector2d(0.0, size.y - radius.y))
              // Draw right line.
              .lineTo(pos + Vector2d(0.0, radius.y))
              // Curve to the top line.
              .curveTo(pos + Vector2d(0.0, radius.y - radius.y * arcMagic),
                       pos + Vector2d(radius.x - radius.x * arcMagic, 0.0),
                       pos + Vector2d(radius.x, 0.0))
              .closePath()
              .build());

    } else {
      // Success: Draw a rect with sharp corners
      return &handle.emplace_or_replace<ComputedPathComponent>(
          PathSpline::Builder()
              .moveTo(pos)
              .lineTo(pos + Vector2d(size.x, 0))
              .lineTo(pos + size)
              .lineTo(pos + Vector2d(0, size.y))
              .closePath()
              .build());
    }
  }

  // Failed: Invalid width or height, don't generate a path.
  handle.remove<ComputedPathComponent>();
  return nullptr;
}

}  // namespace donner::svg::components

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Line>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <line> still has normal attributes, not presentation attributes that can be specified
  // in CSS.
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Path>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  if (name == "d") {
    auto maybeError = components::ParseDFromAttributes(
        handle.get_or_emplace<components::PathComponent>(), params);
    if (maybeError) {
      return std::move(maybeError).value();
    } else {
      // Property found and parsed successfully.
      return true;
    }
  }
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Polygon>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <polygon> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Polyline>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <polyline> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg
