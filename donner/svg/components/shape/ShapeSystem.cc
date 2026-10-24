#include "donner/svg/components/shape/ShapeSystem.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <memory>
#include <tuple>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/GeometryPreparationResourceBudget.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/components/text/TextRootComponent.h"
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

class ScopedTextPathBoundsWorkEpoch {
public:
  explicit ScopedTextPathBoundsWorkEpoch(Registry& registry) {
    auto& context = registry.ctx();
    if (!context.contains<DocumentResourceFamilyContext>()) {
      context.emplace<DocumentResourceFamilyContext>();
    }
    auto& familyContext = context.get<DocumentResourceFamilyContext>();
    if (!familyContext.budget) {
      familyContext.budget = std::make_shared<DocumentResourceFamilyBudget>();
    }
    family_ = familyContext.budget;
    family_->beginTextPathMeasurementWorkEpoch();
  }
  ~ScopedTextPathBoundsWorkEpoch() { family_->endTextPathMeasurementWorkEpoch(); }

  ScopedTextPathBoundsWorkEpoch(const ScopedTextPathBoundsWorkEpoch&) = delete;
  ScopedTextPathBoundsWorkEpoch& operator=(const ScopedTextPathBoundsWorkEpoch&) = delete;

private:
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
};

/// Create a FontMetrics with viewport size and font-size context, so that CSS units
/// (em/ex/ch/rem/vw/vh/vmin/vmax) resolve correctly.
FontMetrics fontMetricsForElement(Registry& registry, const ComputedStyleComponent& style,
                                  Entity entity) {
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
          metrics.rootFontSize = rootStyle->properties->fontSize.get().value().toPixels(
              Box2d(), FontMetrics(), Lengthd::Extent::Mixed);
        }
      }
    }
  }

  // Element's computed font-size for em/ex/ch units.
  if (style.properties) {
    // Resolve font-size using default metrics (font-size itself can't be em-relative to itself).
    metrics.fontSize = style.properties->fontSize.get().value().toPixels(Box2d(), FontMetrics(),
                                                                         Lengthd::Extent::Mixed);
  }

#ifdef DONNER_TEXT_ENABLED
  if (auto* textEngine = registry.ctx().find<TextEngine>()) {
    if (style.properties) {
      if (const auto chUnit =
              textEngine->measureChUnitInEm(style.properties->fontFamily.get().value(), entity)) {
        metrics.chUnitInEm = *chUnit;
      }
    }
  }
#endif

  return metrics;
}

constexpr std::array<std::string_view, 13> kShapeLengthAttributes = {
    "x", "y", "width", "height", "cx", "cy", "r", "rx", "ry", "x1", "y1", "x2", "y2"};

bool HasContextualShapeLengths(EntityHandle handle) {
  return std::any_of(
      kShapeLengthAttributes.begin(), kShapeLengthAttributes.end(), [&](std::string_view name) {
        return ContextualLengthFor(*handle.registry(), handle.entity(), name) != nullptr;
      });
}

Lengthd::Extent ShapeLengthExtent(std::string_view name) {
  if (name == "x" || name == "width" || name == "cx" || name == "rx" || name == "x1" ||
      name == "x2") {
    return Lengthd::Extent::X;
  }
  if (name == "y" || name == "height" || name == "cy" || name == "ry" || name == "y1" ||
      name == "y2") {
    return Lengthd::Extent::Y;
  }
  return Lengthd::Extent::Mixed;
}

bool ReserveShapeStyleWork(EntityHandle handle, const ComputedStyleComponent& style) {
  auto* budget = handle.registry()->ctx().find<AnimationResourceBudget>();
  const auto bytes = style.properties->complexPropertyBytes();
  return budget && bytes <= AnimationResourceBudget::kMaximumContextualWork / 6 &&
         budget->reserveContextualWork(bytes * 6);
}

bool ReserveShapeRecipeWork(EntityHandle handle, const ComputedStyleComponent& style) {
  auto* budget = handle.registry()->ctx().find<AnimationResourceBudget>();
  if (!budget) {
    return false;
  }
  const auto bytes = style.properties->complexPropertyBytes();
  if (bytes > AnimationResourceBudget::kMaximumContextualWork / 6) {
    return false;
  }
  std::size_t work = bytes * 6;
  for (std::string_view name : kShapeLengthAttributes) {
    const auto recipe = ContextualLengthFor(*handle.registry(), handle.entity(), name);
    if (recipe) {
      if (recipe->evaluationWork > AnimationResourceBudget::kMaximumContextualWork - work) {
        return false;
      }
      work += recipe->evaluationWork;
    }
  }
  return budget->reserveContextualWork(work);
}

template <typename Properties>
Lengthd ShapeUnderlyingLength(const Properties& properties, const Property<Lengthd>& property,
                              const AnimationValueContext& context) {
  if (const auto value = property.get()) {
    return *value;
  }
  if constexpr (requires { properties.calculateRx(context.viewBox, context.fontMetrics); }) {
    if (property.name == "rx") {
      return std::get<0>(properties.calculateRx(context.viewBox, context.fontMetrics));
    }
    if (property.name == "ry") {
      return std::get<0>(properties.calculateRy(context.viewBox, context.fontMetrics));
    }
  }
  return Lengthd();
}

template <typename Properties>
bool SampleShapeProperty(EntityHandle handle, const Properties& bases,
                         const Property<Lengthd>& base, Property<Lengthd>& destination,
                         AnimationValueContext context) {
  const auto recipe = ContextualLengthFor(*handle.registry(), handle.entity(), destination.name);
  if (!recipe) {
    return false;
  }
  context.extent = ShapeLengthExtent(destination.name);
  const auto value =
      SampleContextualLengthRecipe(*recipe, ShapeUnderlyingLength(bases, base, context), context);
  if (!value) {
    return false;
  }
  destination.substitute(*value);
  destination.specificity = css::Specificity::Animation();
  return true;
}

template <typename Component, typename Computed>
auto EffectiveShapeProperties(EntityHandle handle, const Component& shape,
                              const ComputedStyleComponent& style, const FontMetrics& metrics,
                              ParseWarningSink& warningSink)
    -> std::optional<decltype(shape.properties)> {
  if (handle.all_of<ShadowEntityComponent>() && !handle.all_of<Computed>() &&
      !ReserveShapeStyleWork(handle, style)) {
    return std::nullopt;
  }
  Computed computed(shape.properties, style.properties->unparsedProperties, warningSink);
  auto bases = computed.properties;
  if (HasContextualShapeLengths(handle) && ReserveShapeRecipeWork(handle, style)) {
    auto properties = shape.properties;
    auto source = bases.allProperties();
    auto destination = properties.allProperties();
    AnimationValueContext context;
    context.viewBox = LayoutSystem().getViewBox(handle);
    context.fontMetrics = metrics;
    bool changed = false;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((changed |=
        SampleShapeProperty(handle, bases, std::get<I>(source), std::get<I>(destination), context)),
       ...);
    }(std::make_index_sequence<std::tuple_size_v<decltype(source)>>{});
    if (changed) {
      computed = Computed(properties, style.properties->unparsedProperties, warningSink);
    }
  }
  auto result = computed.properties;
  handle.emplace_or_replace<Computed>(std::move(computed));
  return result;
}

LineComponent EffectiveLine(EntityHandle handle, const LineComponent& line,
                            const ComputedStyleComponent& style, const FontMetrics& metrics) {
  LineComponent result = line;
  if (!HasContextualShapeLengths(handle) || !ReserveShapeRecipeWork(handle, style)) {
    return result;
  }
  using Field = Lengthd LineComponent::*;
  const std::pair<std::string_view, Field> fields[] = {{"x1", &LineComponent::x1},
                                                       {"y1", &LineComponent::y1},
                                                       {"x2", &LineComponent::x2},
                                                       {"y2", &LineComponent::y2}};
  AnimationValueContext context;
  context.viewBox = LayoutSystem().getViewBox(handle);
  context.fontMetrics = metrics;
  for (const auto& [name, field] : fields) {
    if (const auto recipe = ContextualLengthFor(*handle.registry(), handle.entity(), name)) {
      context.extent = ShapeLengthExtent(name);
      if (const auto value = SampleContextualLengthRecipe(*recipe, line.*field, context)) {
        result.*field = *value;
      }
    }
  }
  return result;
}

bool ReserveComputedPathPayload(EntityHandle handle, const Path& path) {
  auto& registryContext = handle.registry()->ctx();
  if (!registryContext.contains<GeometryPreparationResourceBudget>()) {
    std::shared_ptr<DocumentResourceFamilyBudget> family;
    if (const auto* context = registryContext.find<DocumentResourceFamilyContext>()) {
      family = context->budget;
    }
    registryContext.emplace<GeometryPreparationResourceBudget>(std::move(family));
  }

  const std::optional<std::size_t> retainedBytes = path.retainedBytes();
  return retainedBytes.has_value() &&
         registryContext.get<GeometryPreparationResourceBudget>().reserve(handle.entity(),
                                                                          *retainedBytes);
}

void RemoveComputedPath(EntityHandle handle) {
  handle.remove<ComputedPathComponent>();
  if (auto* budget = handle.registry()->ctx().find<GeometryPreparationResourceBudget>()) {
    budget->release(handle.entity());
  }
}

std::optional<ParseDiagnostic> ParseDFromAttributes(PathComponent& properties,
                                                    const parser::PropertyParseFnParams& params) {
  if (params.explicitState != PropertyState::NotSet) {
    properties.d.set(params.explicitState, params.specificity);
    return std::nullopt;
  }

  if (const std::string_view* str = std::get_if<std::string_view>(&params.valueOrComponents)) {
    properties.d.set(RcString(*str), params.specificity);
  } else {
    auto maybeError = Parse(
        params,
        [](const parser::PropertyParseFnParams& params) {
          return parser::ParsePathDataProperty(params.components());
        },
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
/// signal a precise "geometry actually changed" edge - downstream
/// caches (e.g. the Geode path-encode cache) listen on that signal and rely
/// on it not firing for no-op regenerations.
///
/// \p sourcePathData records the path-data string the spline was parsed from, so a later pass
/// can recognize the spline as still current and skip the parse. Callers that did not produce
/// the spline by parsing path data leave it unset, which clears any key a previous producer
/// left behind - only a parse of path data may leave a key that matches a path-data string.
ComputedPathComponent* emplaceComputedPathIfChanged(
    EntityHandle handle, Path newPath, std::optional<RcString> sourcePathData = std::nullopt) {
  if (!ReserveComputedPathPayload(handle, newPath)) {
    RemoveComputedPath(handle);
    return nullptr;
  }
  if (auto* existing = handle.try_get<ComputedPathComponent>()) {
    if (existing->spline == newPath) {
      // Assign through the reference rather than re-emplacing, so the unchanged-geometry case
      // still does not fire on_update<ComputedPathComponent>.
      existing->sourcePathData = std::move(sourcePathData);
      return existing;
    }
  }

  // Aggregate-initialize the component complete with its key, rather than filling the key in
  // afterwards: `on_construct` and `on_update` listeners run inside `emplace_or_replace`, and a
  // two-step write would let them observe a keyless component. The arguments are positional, so
  // they track ComputedPathComponent's member order.
  return &handle.emplace_or_replace<ComputedPathComponent>(
      std::move(newPath), /*cachedLocalBounds=*/std::nullopt, std::move(sourcePathData));
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

bool HasIdenticalPathData(const Path& first, const Path& second) {
  const auto sameCommand = [](const Path::Command& a, const Path::Command& b) {
    return a.verb == b.verb && a.pointIndex == b.pointIndex && a.isInternal == b.isInternal;
  };
  const auto samePoint = [](const Vector2d& a, const Vector2d& b) {
    return a.x == b.x && a.y == b.y;
  };
  return std::equal(first.commands().begin(), first.commands().end(), second.commands().begin(),
                    second.commands().end(), sameCommand) &&
         std::equal(first.points().begin(), first.points().end(), second.points().begin(),
                    second.points().end(), samePoint);
}

bool IsDisplayNone(EntityHandle handle, ParseWarningSink& warningSink) {
  const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, warningSink);
  return style.properties && style.properties->display.get().value() == Display::None;
}

}  // namespace

EntityHandle ShapeSystem::geometryHandle(EntityHandle instance) {
  const auto* shadow = instance.try_get<ShadowEntityComponent>();
  if (!shadow) {
    return instance;
  }
  const EntityHandle light(*instance.registry(), shadow->lightEntity);
  if (!instance.all_of<ComputedInstanceGeometryComponent>()) {
    return light;
  }
  const auto* instancePath = instance.try_get<ComputedPathComponent>();
  const auto* lightPath = light.try_get<ComputedPathComponent>();
  // Identical instance geometry retains the source's renderer cache and batching identity.
  return instancePath && lightPath && HasIdenticalPathData(instancePath->spline, lightPath->spline)
             ? light
             : instance;
}

void ShapeSystem::instantiateShadowComputedPath(EntityHandle handle,
                                                ParseWarningSink& warningSink) {
  const auto* shadow = handle.try_get<ShadowEntityComponent>();
  const auto* style = handle.try_get<ComputedStyleComponent>();
  if (!shadow || !style || !style->properties) {
    return;
  }
  const EntityHandle light(*handle.registry(), shadow->lightEntity);
  if (!light.any_of<CircleComponent, EllipseComponent, LineComponent, RectComponent>()) {
    if (handle.all_of<ComputedInstanceGeometryComponent>()) {
      RemoveComputedPath(handle);
      handle.remove<ComputedInstanceGeometryComponent, ComputedCircleComponent,
                    ComputedEllipseComponent, ComputedRectComponent>();
    }
    return;
  }
  auto& prepared = handle.get_or_emplace<ComputedInstanceGeometryComponent>();
  if (prepared.prepared) {
    return;
  }
  prepared.prepared = true;
  handle.remove<ComputedCircleComponent, ComputedEllipseComponent, ComputedRectComponent>();
  const FontMetrics metrics = fontMetricsForElement(*handle.registry(), *style, handle.entity());
  ForEachShape<entt::type_list<CircleComponent, EllipseComponent, LineComponent, RectComponent>>(
      [&]<typename Shape>() {
        if (const auto* component = light.try_get<Shape>()) {
          createComputedShapeWithStyle(handle, *component, *style, metrics, warningSink);
          return true;
        }
        return false;
      });
}

void ShapeSystem::instantiateShadowComputedPaths(Registry& registry,
                                                 std::span<const Entity> entities,
                                                 ParseWarningSink& warningSink) {
  for (Entity entity : entities) {
    instantiateShadowComputedPath(EntityHandle(registry, entity), warningSink);
  }
}

ComputedPathComponent* ShapeSystem::createComputedPathIfShape(EntityHandle handle,
                                                              const FontMetrics& fontMetrics,
                                                              ParseWarningSink& warningSink) {
  const EntityHandle geometry = geometryHandle(handle);
  if (geometry.entity() != handle.entity()) {
    return geometry.try_get<ComputedPathComponent>();
  }
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
  for (auto [entity, state] : registry.view<ComputedInstanceGeometryComponent>().each()) {
    state.prepared = false;
  }
  ForEachShape<AllShapes>([&]<typename ShapeType>() {
    for (auto view = registry.view<ShapeType, ComputedStyleComponent>(); auto entity : view) {
      auto [shape, style] = view.get(entity);
      const FontMetrics metrics = fontMetricsForElement(registry, style, entity);
      createComputedShapeWithStyle(EntityHandle(registry, entity), shape, style, metrics,
                                   warningSink);
    }

    const bool shouldExit = false;
    return shouldExit;
  });
  for (Entity entity : registry.view<ShadowEntityComponent, ComputedStyleComponent>()) {
    instantiateShadowComputedPath(EntityHandle(registry, entity), warningSink);
  }
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

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (IsDisplayNone(handle, disabledSink)) {
    return std::nullopt;
  }
  const ScopedTextPathBoundsWorkEpoch textPathWorkEpoch(*handle.registry());

  const auto accumulate = [&overallBounds](const Box2d& bounds) {
    overallBounds = overallBounds ? Box2d::Union(overallBounds.value(), bounds) : bounds;
  };

  // Text geometry is produced by the text engine rather than a ComputedPathComponent, so it has to
  // be accumulated separately. The box is taken at the text root and its subtree is not descended
  // into, because the root's box already spans every span it contains. `visibility: hidden` is not
  // consulted: it suppresses painting only, and hidden text still contributes to the box.
  const auto accumulateTextBounds = [&](EntityHandle entity) {
#ifdef DONNER_TEXT_ENABLED
    if (!entity.all_of<TextRootComponent>()) {
      return;
    }

    auto* textEngine = entity.registry()->ctx().find<TextEngine>();
    if (!textEngine) {
      return;
    }

    const Box2d textBounds = textEngine->computedObjectBoundingBox(entity);
    if (textBounds.isEmpty()) {
      return;
    }

    accumulate((LayoutSystem().getEntityFromWorldTransform(entity) * worldFromTarget)
                   .transformBox(textBounds));
#else
    (void)entity;
#endif
  };

  // Accumulate `handle` and its descendants; the walk visits `handle` itself first. `display: none`
  // removes an element and its whole subtree from the rendering tree, so a hidden element prunes:
  // skipping only the element itself would still let its descendants widen the box.
  donner::components::ForAllChildrenRecursivePruned(
      handle, [this, &disabledSink, &accumulate, &accumulateTextBounds,
               &worldFromTarget](EntityHandle child) {
        if (IsDisplayNone(child, disabledSink)) {
          return false;
        }

        if (ComputedPathComponent* computedPath =
                createComputedPathIfShape(child, FontMetrics(), disabledSink)) {
          accumulate(computedPath->transformedBounds(
              LayoutSystem().getEntityFromWorldTransform(child) * worldFromTarget));
        }
        accumulateTextBounds(child);
        return true;
      });

  return overallBounds;
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const CircleComponent& circle, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const auto effective = EffectiveShapeProperties<CircleComponent, ComputedCircleComponent>(
      handle, circle, style, fontMetrics, warningSink);
  if (!effective) {
    RemoveComputedPath(handle);
    return nullptr;
  }

  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d center(
      effective->cx.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::X),
      effective->cy.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::Y));
  const double radius = effective->r.get().value().toPixels(viewport, fontMetrics);

  if (radius > 0.0) {
    Path path = PathBuilder().addCircle(center, radius).build();

    return emplaceComputedPathIfChanged(handle, std::move(path));
  } else {
    RemoveComputedPath(handle);
    return nullptr;
  }
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const EllipseComponent& ellipse, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const auto effective = EffectiveShapeProperties<EllipseComponent, ComputedEllipseComponent>(
      handle, ellipse, style, fontMetrics, warningSink);
  if (!effective) {
    RemoveComputedPath(handle);
    return nullptr;
  }

  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d center(
      effective->cx.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::X),
      effective->cy.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::Y));
  const Vector2d radius(std::get<1>(effective->calculateRx(viewport, fontMetrics)),
                        std::get<1>(effective->calculateRy(viewport, fontMetrics)));

  if (radius.x > 0.0 && radius.y > 0.0) {
    Path path = PathBuilder().addEllipse(Box2d(center - radius, center + radius)).build();

    return emplaceComputedPathIfChanged(handle, std::move(path));
  } else {
    RemoveComputedPath(handle);
    return nullptr;
  }
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const LineComponent& line, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const LineComponent effective = EffectiveLine(handle, line, style, fontMetrics);
  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d start(effective.x1.toPixels(viewport, fontMetrics, Lengthd::Extent::X),
                       effective.y1.toPixels(viewport, fontMetrics, Lengthd::Extent::Y));
  const Vector2d end(effective.x2.toPixels(viewport, fontMetrics, Lengthd::Extent::X),
                     effective.y2.toPixels(viewport, fontMetrics, Lengthd::Extent::Y));

  Path path = PathBuilder().moveTo(start).lineTo(end).build();
  return emplaceComputedPathIfChanged(handle, std::move(path));
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
        [](const parser::PropertyParseFnParams& params) {
          return parser::ParsePathDataProperty(params.components());
        },
        &actualD);
    if (maybeError) {
      warningSink.add(std::move(maybeError.value()));
      return nullptr;
    }
  }

  if (path.splineOverride) {
    return emplaceComputedPathIfChanged(handle, path.splineOverride.value());
  } else if (actualD.isSpecified()) {
    // `Property::get()` hands back a fresh optional, so this has to be a value rather than a
    // reference into a temporary. Copying an RcString shares its buffer.
    const RcString pathData = actualD.get().value();

    // Reuse the retained spline when it was parsed from this exact string. The shape pass runs
    // over every shape on every prepare, and re-parsing path data that has not changed is the
    // single most expensive thing it does. `pathData` is resolved fresh here from the current
    // presentation attribute and the current computed style, so any change to either - however
    // it was made - produces a different string and falls through to the parse below.
    if (ComputedPathComponent* existing = handle.try_get<ComputedPathComponent>();
        existing && existing->sourcePathData && existing->sourcePathData.value() == pathData) {
      return existing;
    }

    auto maybePath = parser::PathParser::Parse(pathData);
    const bool hadDiagnostic = maybePath.hasError();
    if (hadDiagnostic) {
      // Propagate warnings, which may be set on success too.
      warningSink.add(std::move(maybePath.error()));
    }

    if (maybePath.hasResult() && !maybePath.result().empty()) {
      // Success: Return path. Only remember the source string when the parse was clean; path
      // data that produces a diagnostic must be re-parsed by every pass so the diagnostic keeps
      // reaching the sink for as long as the malformed data is present.
      std::optional<RcString> cacheKey;
      if (!hadDiagnostic) {
        cacheKey = pathData;
      }

      return emplaceComputedPathIfChanged(handle, std::move(maybePath.result()),
                                          std::move(cacheKey));
    }
  }

  // Failed: Could not parse path
  RemoveComputedPath(handle);
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

  return emplaceComputedPathIfChanged(handle, builder.build());
}

ComputedPathComponent* ShapeSystem::createComputedShapeWithStyle(
    EntityHandle handle, const RectComponent& rect, const ComputedStyleComponent& style,
    const FontMetrics& fontMetrics, ParseWarningSink& warningSink) {
  const auto effective = EffectiveShapeProperties<RectComponent, ComputedRectComponent>(
      handle, rect, style, fontMetrics, warningSink);
  if (!effective) {
    RemoveComputedPath(handle);
    return nullptr;
  }

  const Box2d viewport = LayoutSystem().getViewBox(handle);

  const Vector2d pos(
      effective->x.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::X),
      effective->y.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::Y));
  const Vector2d size(
      effective->width.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::X),
      effective->height.get().value().toPixels(viewport, fontMetrics, Lengthd::Extent::Y));

  if (size.x > 0.0 && size.y > 0.0) {
    if (effective->rx.isSpecified() || effective->ry.isSpecified()) {
      const Vector2d radius(
          Clamp(std::get<1>(effective->calculateRx(viewport, fontMetrics)), 0.0, size.x * 0.5),
          Clamp(std::get<1>(effective->calculateRy(viewport, fontMetrics)), 0.0, size.y * 0.5));

      // Success: Draw a rect with rounded corners.
      Path path = PathBuilder().addRoundedRect(Box2d(pos, pos + size), radius.x, radius.y).build();

      return emplaceComputedPathIfChanged(handle, std::move(path));

    } else {
      // Success: Draw a rect with sharp corners
      Path path = PathBuilder().addRect(Box2d(pos, pos + size)).build();

      return emplaceComputedPathIfChanged(handle, std::move(path));
    }
  }

  // Failed: Invalid width or height, don't generate a path.
  RemoveComputedPath(handle);
  return nullptr;
}

ParseResult<bool> ParsePathPresentationAttribute(EntityHandle handle, std::string_view name,
                                                 const parser::PropertyParseFnParams& params) {
  if (name == "d") {
    auto maybeError = ParseDFromAttributes(handle.get_or_emplace<PathComponent>(), params);
    if (maybeError) {
      return std::move(maybeError).value();
    } else {
      // Property found and parsed successfully. Drop any previously instantiated
      // computed path so on-demand readers (computedSpline, worldBounds, editor
      // overlay chrome) see the new geometry instead of a stale cache - matching
      // SVGPathElement::setD().
      RemoveComputedPath(handle);
      return true;
    }
  }
  return false;
}

}  // namespace donner::svg::components
