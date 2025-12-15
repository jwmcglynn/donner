#include "donner/svg/components/shape/ShapeSystem.h"

#include <concepts>
#include <optional>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

namespace {

/**
 * Parse the string of the 'd' presentation attribute out of CSS, which can be parsed with \ref
 * PathParser.
 *
 * @param components CSS ComponentValue list to parse, the right side of the declaration (e.g.
 * `d: <component-values>`).
 * @return parsed string, or an error if the string could not be parsed
 */
ParseResult<RcString> ParseD(std::span<const css::ComponentValue> components) {
  if (auto maybeIdent = parser::TryGetSingleIdent(components);
      maybeIdent && maybeIdent->equalsLowercase("none")) {
    return RcString();
  } else if (components.size() == 1) {
    if (const auto* str = components[0].tryGetToken<css::Token::String>()) {
      return str->value;
    }
  }

  ParseError err;
  err.reason = "Expected string or 'none'";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

std::optional<ParseError> ParseDFromAttributes(PathComponent& properties,
                                               const parser::PropertyParseFnParams& params) {
  if (const std::string_view* str = std::get_if<std::string_view>(&params.valueOrComponents)) {
    properties.d.set(RcString(*str), params.specificity);
  } else {
    auto maybeError = Parse(
        params,
        [](const parser::PropertyParseFnParams& params) { return ParseD(params.components()); },
        &properties.d);
    if (maybeError) {
      return std::move(maybeError.value());
    }
  }

  return std::nullopt;
}

/**
 * Concept for the callback type of \c ForEachShape
 *
 * Matches lambdas with this signature:
 * ```
 * template<typename T>
 * bool callback<T>();
 * ```
 *
 * For example:
 * ```
 * [&]<typename ShapeType>() -> bool {
 *    const auto& shape = registry.get<ShapeType>(entity);
 *    return shouldExit;
 * }
 * ```
 */
template <typename F, typename ComponentType>
concept ForEachCallback = requires(const F& f) {
  { f.template operator()<ComponentType>() } -> std::same_as<bool>;
};

/**
 * Helper to call the callback on each entt::type_list element. Short-circuits if the callback
returns true.
 *
 * @tparam TypeList Tuple of types to iterate over.
 * @tparam F Callback type. Must be callable with a single type parameter.
 * @tparam Indices Indices of the tuple.
 * @param f Callback.
 * @return true if the callback returns true for any element.
 * @return false if the callback returns false for all elements.
 */
template <typename TypeList, typename F, std::size_t... Indices>
  requires(ForEachCallback<F, typename entt::type_list_element<Indices, TypeList>::type> && ...)
constexpr bool ForEachShapeImpl(const F& f, std::index_sequence<Indices...>) {
  return (f.template operator()<typename entt::type_list_element<Indices, TypeList>::type>() ||
          ...);
}

/**
 * Iterate over a tuple at compile time, calling the callback on each element. Short-circuits if
 * the callback returns true.
 *
 * @tparam TypeList Tuple of types to iterate over.
 * @tparam F Callback type. Must be callable with a single type parameter.
 * @param f Callback.
 * @return true if the callback returns true for any element.
 */
template <typename TypeList, typename F>
constexpr bool ForEachShape(const F& f) {
  return ForEachShapeImpl<TypeList>(f, std::make_index_sequence<TypeList::size>{});
}

}  // namespace

ComputedPathComponent* ShapeSystem::createComputedPathIfShape(
    EntityHandle handle, const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  ComputedPathComponent* computedPath = handle.try_get<ComputedPathComponent>();
  if (computedPath) {
    return computedPath;
  }

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

std::optional<Boxd> ShapeSystem::getShapeBounds(EntityHandle handle) {
  const Transformd worldFromOuterEntityLocal =
      LayoutSystem().getEntityFromWorldTransform(handle).inverse();

  return getTransformedShapeBounds(handle, worldFromOuterEntityLocal);
}

std::optional<Boxd> ShapeSystem::getShapeWorldBounds(EntityHandle handle) {
  return getTransformedShapeBounds(handle, Transformd());
}

bool ShapeSystem::pathFillIntersects(EntityHandle handle, const Vector2d& point,
                                     FillRule fillRule) {
  if (ComputedPathComponent* computedPath =
          createComputedPathIfShape(handle, FontMetrics(), nullptr)) {
    return computedPath->spline.isInside(point, fillRule);
  }

  return false;
}

bool ShapeSystem::pathStrokeIntersects(EntityHandle handle, const Vector2d& point,
                                       double strokeWidth) {
  if (ComputedPathComponent* computedPath =
          createComputedPathIfShape(handle, FontMetrics(), nullptr)) {
    return computedPath->spline.isOnPath(point, strokeWidth);
  }

  return false;
}

std::optional<Boxd> ShapeSystem::getTransformedShapeBounds(EntityHandle handle,
                                                           const Transformd& worldFromTarget) {
  std::optional<Boxd> overallBounds;

  if (const ComputedStyleComponent* style = handle.try_get<ComputedStyleComponent>()) {
    if (style->properties->display.getRequired() == Display::None) {
      return std::nullopt;
    }
  }

  if (ComputedPathComponent* computedPath =
          createComputedPathIfShape(handle, FontMetrics(), nullptr)) {
    overallBounds = computedPath->transformedBounds(
        LayoutSystem().getEntityFromWorldTransform(handle) * worldFromTarget);
  }

  // Iterate over all children and accumulate their bounds.
  donner::components::ForAllChildrenRecursive(
      handle, [this, &overallBounds, &worldFromTarget](EntityHandle child) {
        if (const ComputedStyleComponent* style = child.try_get<ComputedStyleComponent>()) {
          if (style->properties->display.getRequired() == Display::None) {
            return;
          }
        }

        if (ComputedPathComponent* computedPath =
                createComputedPathIfShape(child, FontMetrics(), nullptr)) {
          const Boxd bounds = computedPath->transformedBounds(
              LayoutSystem().getEntityFromWorldTransform(child) * worldFromTarget);
          overallBounds = overallBounds ? Boxd::Union(overallBounds.value(), bounds) : bounds;
        }
      });

  return overallBounds;
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const CircleComponent& circle, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedCircleComponent& computedCircle = handle.get_or_emplace<ComputedCircleComponent>(
      circle.properties, style.properties->unparsedProperties, outWarnings);

  const Boxd viewport = LayoutSystem().getViewBox(handle);

  const Vector2d center(computedCircle.properties.cx.getRequired().toPixels(viewport, fontMetrics,
                                                                            Lengthd::Extent::X),
                        computedCircle.properties.cy.getRequired().toPixels(viewport, fontMetrics,
                                                                            Lengthd::Extent::Y));
  const double radius = computedCircle.properties.r.getRequired().toPixels(viewport, fontMetrics);

  if (radius > 0.0) {
    PathSpline path;
    path.circle(center, radius);

    return &handle.emplace_or_replace<ComputedPathComponent>(std::move(path));
  } else {
    return nullptr;
  }
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const EllipseComponent& ellipse, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedEllipseComponent& computedEllipse = handle.get_or_emplace<ComputedEllipseComponent>(
      ellipse.properties, style.properties->unparsedProperties, outWarnings);

  const Boxd viewport = LayoutSystem().getViewBox(handle);

  const Vector2d center(
      computedEllipse.properties.cx.getRequired().toPixels(viewport, fontMetrics),
      computedEllipse.properties.cy.getRequired().toPixels(viewport, fontMetrics));
  const Vector2d radius(std::get<1>(computedEllipse.properties.calculateRx(viewport, fontMetrics)),
                        std::get<1>(computedEllipse.properties.calculateRy(viewport, fontMetrics)));

  if (radius.x > 0.0 && radius.y > 0.0) {
    PathSpline path;
    path.ellipse(center, radius);

    return &handle.emplace_or_replace<ComputedPathComponent>(std::move(path));
  } else {
    return nullptr;
  }
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const LineComponent& line, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const Boxd viewport = LayoutSystem().getViewBox(handle);

  const Vector2d start(line.x1.toPixels(viewport, fontMetrics),
                       line.y1.toPixels(viewport, fontMetrics));
  const Vector2d end(line.x2.toPixels(viewport, fontMetrics),
                     line.y2.toPixels(viewport, fontMetrics));

  PathSpline path;
  path.moveTo(start);
  path.lineTo(end);
  return &handle.emplace_or_replace<ComputedPathComponent>(std::move(path));
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const PathComponent& path, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  Property<RcString> actualD = path.d;
  const auto& properties = style.properties->unparsedProperties;
  if (auto it = properties.find("d"); it != properties.end()) {
    auto maybeError = Parse(
        parser::PropertyParseFnParams::Create(it->second.declaration, it->second.specificity,
                                              parser::PropertyParseBehavior::AllowUserUnits),
        [](const parser::PropertyParseFnParams& params) { return ParseD(params.components()); },
        &actualD);
    if (maybeError) {
      if (outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
      return nullptr;
    }
  }

  if (path.splineOverride) {
    return &handle.emplace_or_replace<ComputedPathComponent>(path.splineOverride.value());
  } else if (actualD.hasValue()) {
    std::optional<ParseError> pathWarning;
    auto maybePath = parser::PathParser::Parse(actualD.get().value(), &pathWarning);
    if (pathWarning.has_value() && outWarnings != nullptr) {
      outWarnings->emplace_back(std::move(pathWarning.value()));
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

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const PolyComponent& poly, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  PathSpline path;

  if (!poly.points.empty()) {
    path.moveTo(poly.points[0]);
  }

  for (size_t i = 1; i < poly.points.size(); ++i) {
    path.lineTo(poly.points[i]);
  }

  if (poly.type == PolyComponent::Type::Polygon) {
    path.closePath();
  }

  return &handle.emplace_or_replace<ComputedPathComponent>(std::move(path));
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const RectComponent& rect, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, std::vector<ParseError>* outWarnings) {
  const ComputedRectComponent& computedRect = handle.get_or_emplace<ComputedRectComponent>(
      rect.properties, style.properties->unparsedProperties, outWarnings);

  const Boxd viewport = LayoutSystem().getViewBox(handle);

  const Vector2d pos(
      computedRect.properties.x.getRequired().toPixels(viewport, fontMetrics, Lengthd::Extent::X),
      computedRect.properties.y.getRequired().toPixels(viewport, fontMetrics, Lengthd::Extent::Y));
  const Vector2d size(computedRect.properties.width.getRequired().toPixels(viewport, fontMetrics,
                                                                           Lengthd::Extent::X),
                      computedRect.properties.height.getRequired().toPixels(viewport, fontMetrics,
                                                                            Lengthd::Extent::Y));

  if (size.x > 0.0 && size.y > 0.0) {
    if (computedRect.properties.rx.hasValue() || computedRect.properties.ry.hasValue()) {
      // 4/3 * (1 - cos(45 deg) / sin(45 deg) = 4/3 * (sqrt 2) - 1
      const double arcMagic = 0.5522847498;
      const Vector2d radius(
          Clamp(std::get<1>(computedRect.properties.calculateRx(viewport, fontMetrics)), 0.0,
                size.x * 0.5),
          Clamp(std::get<1>(computedRect.properties.calculateRy(viewport, fontMetrics)), 0.0,
                size.y * 0.5));

      // Success: Draw a rect with rounded corners.
      PathSpline path;

      // Draw top line.
      path.moveTo(pos + Vector2d(radius.x, 0));
      path.lineTo(pos + Vector2d(size.x - radius.x, 0.0));
      // Curve to the right line.
      path.curveTo(pos + Vector2d(size.x - radius.x + radius.x * arcMagic, 0.0),
                   pos + Vector2d(size.x, radius.y - radius.y * arcMagic),
                   pos + Vector2d(size.x, radius.y));
      // Draw right line.
      path.lineTo(pos + Vector2d(size.x, size.y - radius.y));
      // Curve to the bottom line.
      path.curveTo(pos + Vector2d(size.x, size.y - radius.y + radius.y * arcMagic),
                   pos + Vector2d(size.x - radius.x + radius.x * arcMagic, size.y),
                   pos + Vector2d(size.x - radius.x, size.y));
      // Draw bottom line.
      path.lineTo(pos + Vector2d(radius.x, size.y));
      // Curve to the left line.
      path.curveTo(pos + Vector2d(radius.x - radius.x * arcMagic, size.y),
                   pos + Vector2d(0.0, size.y - radius.y + radius.y * arcMagic),
                   pos + Vector2d(0.0, size.y - radius.y));
      // Draw right line.
      path.lineTo(pos + Vector2d(0.0, radius.y));
      // Curve to the top line.
      path.curveTo(pos + Vector2d(0.0, radius.y - radius.y * arcMagic),
                   pos + Vector2d(radius.x - radius.x * arcMagic, 0.0),
                   pos + Vector2d(radius.x, 0.0));
      path.closePath();

      return &handle.emplace_or_replace<ComputedPathComponent>(std::move(path));

    } else {
      // Success: Draw a rect with sharp corners
      PathSpline path;
      path.moveTo(pos);
      path.lineTo(pos + Vector2d(size.x, 0));
      path.lineTo(pos + size);
      path.lineTo(pos + Vector2d(0, size.y));
      path.closePath();

      return &handle.emplace_or_replace<ComputedPathComponent>(std::move(path));
    }
  }

  // Failed: Invalid width or height, don't generate a path.
  handle.remove<ComputedPathComponent>();
  return nullptr;
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

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

}  // namespace donner::svg::parser
