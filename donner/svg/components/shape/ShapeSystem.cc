#include "donner/svg/components/shape/ShapeSystem.h"

#include <concepts>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/text/TextEngine.h"
#endif
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

namespace {

/// Create a FontMetrics with viewport size and font-size context, so that CSS units
/// (em/ex/ch/rem/vw/vh/vmin/vmax) resolve correctly.
FontMetrics fontMetricsForElement(Registry& registry, const ComputedStyleComponent& style) {
  FontMetrics metrics;

  if (auto* ctx = registry.ctx().find<SVGDocumentContext>()) {
    // Viewport size for vw/vh/vmin/vmax.
    if (ctx->canvasSize) {
      metrics.viewportSize = Vector2d(static_cast<double>(ctx->canvasSize->x),
                                      static_cast<double>(ctx->canvasSize->y));
    }

    // Root font-size for rem units.
    if (ctx->rootEntity != entt::null) {
      if (const auto* rootStyle = registry.try_get<ComputedStyleComponent>(ctx->rootEntity)) {
        if (rootStyle->properties) {
          // Use default FontMetrics for resolving root font-size (avoids circular dependency).
          metrics.rootFontSize = rootStyle->properties->fontSize.getRequired().toPixels(
              Box2d(), FontMetrics(), Lengthd::Extent::Mixed);
        }
      }
    }
  }

  // Element's computed font-size for em/ex/ch units.
  if (style.properties) {
    // Resolve font-size using default metrics (font-size itself can't be em-relative to itself).
    metrics.fontSize = style.properties->fontSize.getRequired().toPixels(Box2d(), FontMetrics(),
                                                                         Lengthd::Extent::Mixed);
  }

#ifdef DONNER_TEXT_ENABLED
  if (auto* textEngine = registry.ctx().find<TextEngine>()) {
    if (style.properties) {
      if (const auto chUnit =
              textEngine->measureChUnitInEm(style.properties->fontFamily.getRequired())) {
        metrics.chUnitInEm = *chUnit;
      }
    }
  }
#endif

  return metrics;
}

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

  ParseDiagnostic err;
  err.reason = "Expected string or 'none'";
  err.range.start = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

std::optional<ParseDiagnostic> ParseDFromAttributes(PathComponent& properties,
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

/// Emplace or replace ComputedPathComponent only if the newly computed
/// path differs from any existing one. Suppressing the write when the
/// geometry is unchanged keeps entt's on_update<ComputedPathComponent>
/// signal a precise "geometry actually changed" edge — downstream
/// caches (e.g. the Geode encode cache from design doc 0030
/// Milestone 2) listen on that signal and rely on it not firing for
/// no-op regenerations.
ComputedPathComponent& emplaceComputedPathIfChanged(EntityHandle handle, Path newPath) {
  if (auto* existing = handle.try_get<ComputedPathComponent>()) {
    if (existing->spline == newPath) {
      return *existing;
    }
  }
  return handle.emplace_or_replace<ComputedPathComponent>(std::move(newPath));
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

ComputedPathComponent* ShapeSystem::createComputedPathIfShape(EntityHandle handle,
                                                              const FontMetrics& fontMetrics,
                                                              ParseWarningSink& warningSink) {
  ComputedPathComponent* computedPath = handle.try_get<ComputedPathComponent>();
  if (computedPath) {
    return computedPath;
  }

  ForEachShape<AllShapes>([&]<typename ShapeType>() -> bool {
    using ShapeComponent = ShapeType;
    bool shouldExit = false;

    if (handle.all_of<ShapeComponent>()) {
      const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, warningSink);
      computedPath = createComputedShapeWithStyle(handle, handle.get<ShapeComponent>(), style,
                                                  fontMetrics, warningSink);
      // Note the computedPath may be null if the shape failed to instantiate due to an error (like
      // having zero points), when this occurs no other shapes will match and we should exit early.
      shouldExit = true;
    }

    return shouldExit;
  });

  return computedPath;
}

void ShapeSystem::instantiateAllComputedPaths(Registry& registry, ParseWarningSink& warningSink) {
  ForEachShape<AllShapes>([&]<typename ShapeType>() {
    for (auto view = registry.view<ShapeType, ComputedStyleComponent>(); auto entity : view) {
      auto [shape, style] = view.get(entity);
      const FontMetrics metrics = fontMetricsForElement(registry, style);
      createComputedShapeWithStyle(EntityHandle(registry, entity), shape, style, metrics,
                                   warningSink);
    }

    const bool shouldExit = false;
    return shouldExit;
  });
}

std::optional<Box2d> ShapeSystem::getShapeBounds(EntityHandle handle) {
  const Transform2d worldFromOuterEntityLocal =
      LayoutSystem().getEntityFromWorldTransform(handle).inverse();

  return getTransformedShapeBounds(handle, worldFromOuterEntityLocal);
}

std::optional<Box2d> ShapeSystem::getShapeWorldBounds(EntityHandle handle) {
  return getTransformedShapeBounds(handle, Transform2d());
}

bool ShapeSystem::pathFillIntersects(EntityHandle handle, const Vector2d& point,
                                     FillRule fillRule) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (ComputedPathComponent* computedPath =
          createComputedPathIfShape(handle, FontMetrics(), disabledSink)) {
    return computedPath->spline.isInside(point, fillRule);
  }

  return false;
}

bool ShapeSystem::pathStrokeIntersects(EntityHandle handle, const Vector2d& point,
                                       double strokeWidth) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (ComputedPathComponent* computedPath =
          createComputedPathIfShape(handle, FontMetrics(), disabledSink)) {
    return computedPath->spline.isOnPath(point, strokeWidth);
  }

  return false;
}

std::optional<Box2d> ShapeSystem::getTransformedShapeBounds(EntityHandle handle,
                                                            const Transform2d& worldFromTarget) {
  std::optional<Box2d> overallBounds;

  if (const ComputedStyleComponent* style = handle.try_get<ComputedStyleComponent>()) {
    if (style->properties->display.getRequired() == Display::None) {
      return std::nullopt;
    }
  }

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (ComputedPathComponent* computedPath =
          createComputedPathIfShape(handle, FontMetrics(), disabledSink)) {
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

        ParseWarningSink disabledSink = ParseWarningSink::Disabled();
        if (ComputedPathComponent* computedPath =
                createComputedPathIfShape(child, FontMetrics(), disabledSink)) {
          const Box2d bounds = computedPath->transformedBounds(
              LayoutSystem().getEntityFromWorldTransform(child) * worldFromTarget);
          overallBounds = overallBounds ? Box2d::Union(overallBounds.value(), bounds) : bounds;
        }
      });

  return overallBounds;
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const CircleComponent& circle, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const ComputedCircleComponent& computedCircle = handle.get_or_emplace<ComputedCircleComponent>(
      circle.properties, style.properties->unparsedProperties, warningSink);

  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d center(computedCircle.properties.cx.getRequired().toPixels(viewport, fontMetrics,
                                                                            Lengthd::Extent::X),
                        computedCircle.properties.cy.getRequired().toPixels(viewport, fontMetrics,
                                                                            Lengthd::Extent::Y));
  const double radius = computedCircle.properties.r.getRequired().toPixels(viewport, fontMetrics);

  if (radius > 0.0) {
    Path path = PathBuilder().addCircle(center, radius).build();

    return &emplaceComputedPathIfChanged(handle, std::move(path));
  } else {
    return nullptr;
  }
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const EllipseComponent& ellipse, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const ComputedEllipseComponent& computedEllipse = handle.get_or_emplace<ComputedEllipseComponent>(
      ellipse.properties, style.properties->unparsedProperties, warningSink);

  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d center(
      computedEllipse.properties.cx.getRequired().toPixels(viewport, fontMetrics),
      computedEllipse.properties.cy.getRequired().toPixels(viewport, fontMetrics));
  const Vector2d radius(std::get<1>(computedEllipse.properties.calculateRx(viewport, fontMetrics)),
                        std::get<1>(computedEllipse.properties.calculateRy(viewport, fontMetrics)));

  if (radius.x > 0.0 && radius.y > 0.0) {
    Path path = PathBuilder().addEllipse(Box2d(center - radius, center + radius)).build();

    return &emplaceComputedPathIfChanged(handle, std::move(path));
  } else {
    return nullptr;
  }
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const LineComponent& line, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d start(line.x1.toPixels(viewport, fontMetrics),
                       line.y1.toPixels(viewport, fontMetrics));
  const Vector2d end(line.x2.toPixels(viewport, fontMetrics),
                     line.y2.toPixels(viewport, fontMetrics));

  Path path = PathBuilder().moveTo(start).lineTo(end).build();
  return &emplaceComputedPathIfChanged(handle, std::move(path));
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const PathComponent& path, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  Property<RcString> actualD = path.d;
  const auto& properties = style.properties->unparsedProperties;
  if (auto it = properties.find("d"); it != properties.end()) {
    auto maybeError = Parse(
        parser::PropertyParseFnParams::Create(it->second.declaration, it->second.specificity,
                                              parser::PropertyParseBehavior::AllowUserUnits),
        [](const parser::PropertyParseFnParams& params) { return ParseD(params.components()); },
        &actualD);
    if (maybeError) {
      warningSink.add(std::move(maybeError.value()));
      return nullptr;
    }
  }

  if (path.splineOverride) {
    return &emplaceComputedPathIfChanged(handle, path.splineOverride.value());
  } else if (actualD.hasValue()) {
    auto maybePath = parser::PathParser::Parse(actualD.get().value());
    if (maybePath.hasError()) {
      // Propagate warnings, which may be set on success too.
      warningSink.add(std::move(maybePath.error()));
    }

    if (maybePath.hasResult() && !maybePath.result().empty()) {
      // Success: Return path
      return &emplaceComputedPathIfChanged(handle, std::move(maybePath.result()));
    }
  }

  // Failed: Could not parse path
  handle.remove<ComputedPathComponent>();
  return nullptr;
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const PolyComponent& poly, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  PathBuilder builder;

  if (!poly.points.empty()) {
    builder.moveTo(poly.points[0]);
  }

  for (size_t i = 1; i < poly.points.size(); ++i) {
    builder.lineTo(poly.points[i]);
  }

  if (poly.type == PolyComponent::Type::Polygon) {
    builder.closePath();
  }

  return &emplaceComputedPathIfChanged(handle, builder.build());
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const RectComponent& rect, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const ComputedRectComponent& computedRect = handle.get_or_emplace<ComputedRectComponent>(
      rect.properties, style.properties->unparsedProperties, warningSink);

  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d pos(
      computedRect.properties.x.getRequired().toPixels(viewport, fontMetrics, Lengthd::Extent::X),
      computedRect.properties.y.getRequired().toPixels(viewport, fontMetrics, Lengthd::Extent::Y));
  const Vector2d size(computedRect.properties.width.getRequired().toPixels(viewport, fontMetrics,
                                                                           Lengthd::Extent::X),
                      computedRect.properties.height.getRequired().toPixels(viewport, fontMetrics,
                                                                            Lengthd::Extent::Y));

  if (size.x > 0.0 && size.y > 0.0) {
    if (computedRect.properties.rx.hasValue() || computedRect.properties.ry.hasValue()) {
      const Vector2d radius(
          Clamp(std::get<1>(computedRect.properties.calculateRx(viewport, fontMetrics)), 0.0,
                size.x * 0.5),
          Clamp(std::get<1>(computedRect.properties.calculateRy(viewport, fontMetrics)), 0.0,
                size.y * 0.5));

      // Success: Draw a rect with rounded corners.
      Path path = PathBuilder().addRoundedRect(Box2d(pos, pos + size), radius.x, radius.y).build();

      return &emplaceComputedPathIfChanged(handle, std::move(path));

    } else {
      // Success: Draw a rect with sharp corners
      Path path = PathBuilder().addRect(Box2d(pos, pos + size)).build();

      return &emplaceComputedPathIfChanged(handle, std::move(path));
    }
  }

  // Failed: Invalid width or height, don't generate a path.
  handle.remove<ComputedPathComponent>();
  return nullptr;
}

ParseResult<bool> ParsePathPresentationAttribute(EntityHandle handle, std::string_view name,
                                                 const parser::PropertyParseFnParams& params) {
  if (name == "d") {
    auto maybeError = ParseDFromAttributes(handle.get_or_emplace<PathComponent>(), params);
    if (maybeError) {
      return std::move(maybeError).value();
    } else {
      // Property found and parsed successfully.
      return true;
    }
  }
  return false;
}

}  // namespace donner::svg::components
