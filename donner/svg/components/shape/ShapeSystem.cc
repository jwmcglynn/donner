#include "donner/svg/components/shape/ShapeSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/GeometryPreparationResourceBudget.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/text/TextEngine.h"
#endif
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/core/NonScalingStroke.h"
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

LineCap ToLineCap(StrokeLinecap cap) {
  switch (cap) {
    case StrokeLinecap::Butt: return LineCap::Butt;
    case StrokeLinecap::Round: return LineCap::Round;
    case StrokeLinecap::Square: return LineCap::Square;
  }
  return LineCap::Butt;
}

LineJoin ToLineJoin(StrokeLinejoin join) {
  switch (join) {
    case StrokeLinejoin::Miter: return LineJoin::Miter;
    case StrokeLinejoin::MiterClip: return LineJoin::MiterClip;
    case StrokeLinejoin::Round: return LineJoin::Round;
    case StrokeLinejoin::Bevel: return LineJoin::Bevel;
    case StrokeLinejoin::Arcs: return LineJoin::Miter;
  }
  return LineJoin::Miter;
}

void RejectGeometry(CircleClipGeometryBudget* budget, bool& failed) {
  failed = true;
  if (budget) {
    budget->reject();
  }
}

bool IsFiniteBox(const Box2d& box) {
  return std::isfinite(box.topLeft.x) && std::isfinite(box.topLeft.y) &&
         std::isfinite(box.bottomRight.x) && std::isfinite(box.bottomRight.y);
}

std::optional<StrokeStyle> StrokeStyleFor(EntityHandle handle, double widthScale,
                                          CircleClipGeometryBudget* budget, bool& failed) {
  const auto* style = handle.try_get<ComputedStyleComponent>();
  if (!style || !style->properties ||
      style->properties->stroke.get().value() == PaintServer::None()) {
    return std::nullopt;
  }
  const auto& properties = *style->properties;
  const FontMetrics metrics = fontMetricsForElement(*handle.registry(), *style, handle.entity());
  const double width = properties.strokeWidth.get().value().toPixels(
                           LayoutSystem().getViewBox(handle), metrics, Lengthd::Extent::Mixed) *
                       widthScale;
  if (!std::isfinite(width)) {
    RejectGeometry(budget, failed);
    return std::nullopt;
  }
  if (!(width > 0.0)) {
    return std::nullopt;
  }

  return StrokeStyle{.width = width,
                     .cap = ToLineCap(properties.strokeLinecap.get().value()),
                     .join = ToLineJoin(properties.strokeLinejoin.get().value()),
                     .miterLimit = properties.strokeMiterlimit.get().value()};
}

bool ReservePathGeometry(CircleClipGeometryBudget* budget, const Path& path) {
  // Reserve separately so a combined command/point count cannot overflow before admission.
  return !budget ||
         (budget->reserve(path.commands().size()) && budget->reserve(path.points().size()));
}

const Path* PrepareStrokeSource(const Path& centerline, const Transform2d& worldFromGeometry,
                                bool hostSpace, CircleClipGeometryBudget* budget, bool& failed,
                                std::optional<Path>& transformed) {
  if (!ReservePathGeometry(budget, centerline)) {
    RejectGeometry(budget, failed);
    return nullptr;
  }
  if (hostSpace) {
    transformed = centerline.transformed(worldFromGeometry);
  }
  const Path* source = transformed ? &*transformed : &centerline;
  if (!IsFiniteBox(source->bounds())) {
    RejectGeometry(budget, failed);
    return nullptr;
  }
  return source;
}

void RejectPositiveExtentEmptyOutline(const Path& source, CircleClipGeometryBudget* budget,
                                      bool& failed) {
  const Box2d bounds = source.bounds();
  if (bounds.width() > 0.0 || bounds.height() > 0.0) {
    RejectGeometry(budget, failed);
  }
}

std::optional<Box2d> StrokeBounds(EntityHandle styleHandle, const Path& centerline,
                                  const Transform2d& worldFromGeometry,
                                  const Transform2d& targetFromWorld,
                                  CircleClipGeometryBudget* budget, bool geometryMustStayLocal,
                                  bool& failed) {
  const auto* style = styleHandle.try_get<ComputedStyleComponent>();
  if (!style || !style->properties) {
    return std::nullopt;
  }
  const NonScalingStrokeMode mode =
      ResolveNonScalingStrokeMode(style->properties->vectorEffect.getOr(VectorEffect::None),
                                  worldFromGeometry, geometryMustStayLocal);
  const double authoredWidthScale = mode == NonScalingStrokeMode::ScaledLocal
                                        ? EffectiveStrokeWidth(1.0, mode, worldFromGeometry)
                                        : 1.0;
  const auto strokeStyle = StrokeStyleFor(styleHandle, authoredWidthScale, budget, failed);
  if (!strokeStyle) {
    return std::nullopt;
  }

  const bool hostSpace = mode == NonScalingStrokeMode::HostSpace;
  std::optional<Path> hostCenterline;
  const Path* strokeSource =
      PrepareStrokeSource(centerline, worldFromGeometry, hostSpace, budget, failed, hostCenterline);
  if (!strokeSource) {
    return std::nullopt;
  }
  const Path outline = strokeSource->strokeToFill(*strokeStyle, Path::kLocalFlattenTolerance);
  if (outline.empty()) {
    RejectPositiveExtentEmptyOutline(*strokeSource, budget, failed);
    return std::nullopt;
  }
  if (!ReservePathGeometry(budget, outline)) {
    failed = true;
    return std::nullopt;
  }
  const Transform2d targetFromOutline =
      hostSpace ? targetFromWorld : worldFromGeometry * targetFromWorld;
  return outline.transformed(targetFromOutline).bounds();
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

double CircleRadius(const CircleClipPath& circle, const Box2d& box, double x, double y,
                    const FontMetrics& metrics) {
  if (circle.radiusType == CircleClipPath::Radius::Length) {
    return circle.radius.toPixels(box, metrics, Lengthd::Extent::Mixed);
  }
  const std::array<double, 4> distances = {std::abs(x), std::abs(box.width() - x), std::abs(y),
                                           std::abs(box.height() - y)};
  return circle.radiusType == CircleClipPath::Radius::ClosestSide
             ? *std::min_element(distances.begin(), distances.end())
             : *std::max_element(distances.begin(), distances.end());
}

double EllipseRadius(RadialClipPath::Radius type, const Lengthd& radius, const Box2d& box,
                     const FontMetrics& metrics, Lengthd::Extent axis, double position) {
  if (type == RadialClipPath::Radius::Length) {
    return radius.toPixels(box, metrics, axis);
  }
  const double size = axis == Lengthd::Extent::X ? box.width() : box.height();
  return type == RadialClipPath::Radius::ClosestSide
             ? std::min(std::abs(position), std::abs(size - position))
             : std::max(std::abs(position), std::abs(size - position));
}

bool IsFiniteCircle(const Vector2d& center, double radius) {
  return radius > 0.0 && std::isfinite(center.x - radius) && std::isfinite(center.x + radius) &&
         std::isfinite(center.y - radius) && std::isfinite(center.y + radius);
}

bool IsFiniteEllipse(const Vector2d& center, double rx, double ry) {
  // The path builder also adds endpoints and subtracts bounds to obtain its center and radii.
  const std::array<double, 8> coordinates = {center.x - rx, center.x + rx, center.y - ry,
                                             center.y + ry, center.x * 2,  center.y * 2,
                                             rx * 2,        ry * 2};
  return rx > 0.0 && ry > 0.0 &&
         std::all_of(coordinates.begin(), coordinates.end(),
                     [](double value) { return std::isfinite(value); });
}

bool IsRenderableClipCoordinate(double value) {
  return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

using InsetCornerRadii = std::array<Vector2d, 4>;

/// Limit the used largest radius without overflowing sums or underflowing a common factor.
double InsetRadiusLimit(double largest, double first, double second, double side) {
  const double pairLargest = std::max(first, second);
  if (pairLargest == 0) {
    return largest;
  }
  const double sum = first / pairLargest + second / pairLargest;
  int largestExponent, sideExponent, pairExponent, adjustment;
  const double largestMantissa = std::frexp(largest, &largestExponent);
  const double sideMantissa = std::frexp(side, &sideExponent);
  const double pairMantissa = std::frexp(pairLargest, &pairExponent);
  const double mantissa =
      std::frexp(largestMantissa * sideMantissa / (pairMantissa * sum), &adjustment);
  const int exponent = largestExponent + sideExponent - pairExponent + adjustment;
  if (exponent > largestExponent || (exponent == largestExponent && mantissa >= largestMantissa)) {
    return largest;
  }
  return std::ldexp(mantissa, exponent);
}

double ScaleInsetRadius(double radius, double largest, double usedLargest) {
  if (usedLargest >= 1) {
    return radius * (usedLargest / largest);
  }
  return (radius / largest) * usedLargest;
}

void ConstrainInsetRadii(InsetCornerRadii& radii, const Vector2d& size) {
  double largest = 0;
  for (const Vector2d& radius : radii) {
    largest = std::max({largest, radius.x, radius.y});
  }
  if (largest == 0) {
    return;
  }
  const double usedLargest =
      std::min({largest, InsetRadiusLimit(largest, radii[0].x, radii[1].x, size.x),
                InsetRadiusLimit(largest, radii[1].y, radii[2].y, size.y),
                InsetRadiusLimit(largest, radii[2].x, radii[3].x, size.x),
                InsetRadiusLimit(largest, radii[3].y, radii[0].y, size.y)});
  for (Vector2d& radius : radii) {
    if (usedLargest < largest) {
      radius.x = ScaleInsetRadius(radius.x, largest, usedLargest);
      radius.y = ScaleInsetRadius(radius.y, largest, usedLargest);
    }
    if (radius.x == 0 || radius.y == 0) {
      radius = Vector2d();
    }
  }
}

double InsetRadiusPixels(const Lengthd& radius, const Box2d& reference, const FontMetrics& metrics,
                         Lengthd::Extent axis) {
  if (radius.unit != Lengthd::Unit::Percent) {
    return radius.toPixels(reference, metrics, axis);
  }
  const double extent = axis == Lengthd::Extent::X ? reference.width() : reference.height();
  if (!std::isfinite(radius.value) || !std::isfinite(extent)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  int radiusExponent, extentExponent;
  const double radiusMantissa = std::frexp(radius.value, &radiusExponent);
  const double extentMantissa = std::frexp(extent, &extentExponent);
  return std::ldexp(radiusMantissa * extentMantissa / 100, radiusExponent + extentExponent);
}

std::optional<InsetCornerRadii> ResolveInsetRadii(const InsetClipPath& inset,
                                                  const Box2d& reference,
                                                  const FontMetrics& metrics,
                                                  const Vector2d& size) {
  InsetCornerRadii radii;
  for (std::size_t i = 0; i < radii.size(); ++i) {
    radii[i] = Vector2d(InsetRadiusPixels(inset.radiiX[i], reference, metrics, Lengthd::Extent::X),
                        InsetRadiusPixels(inset.radiiY[i], reference, metrics, Lengthd::Extent::Y));
    if (!std::isfinite(radii[i].x) || !std::isfinite(radii[i].y) || radii[i].x < 0 ||
        radii[i].y < 0) {
      return std::nullopt;
    }
  }
  ConstrainInsetRadii(radii, size);
  return radii;
}

Path BuildRoundedInsetClip(const Box2d& rect, const InsetCornerRadii& radii) {
  if (std::all_of(radii.begin(), radii.end(),
                  [](const Vector2d& radius) { return radius.x == 0 && radius.y == 0; })) {
    return PathBuilder(4).addRect(rect).build();
  }
  constexpr double kKappa = 0.5522847498;
  const double left = rect.topLeft.x, top = rect.topLeft.y;
  const double right = rect.bottomRight.x, bottom = rect.bottomRight.y;
  const Vector2d& tl = radii[0];
  const Vector2d& tr = radii[1];
  const Vector2d& br = radii[2];
  const Vector2d& bl = radii[3];
  PathBuilder builder(17);
  builder.moveTo({left + tl.x, top}).lineTo({right - tr.x, top});
  if (tr.x > 0) {
    builder.curveTo({right - tr.x + tr.x * kKappa, top}, {right, top + tr.y - tr.y * kKappa},
                    {right, top + tr.y});
  }
  builder.lineTo({right, bottom - br.y});
  if (br.x > 0) {
    builder.curveTo({right, bottom - br.y + br.y * kKappa}, {right - br.x + br.x * kKappa, bottom},
                    {right - br.x, bottom});
  }
  builder.lineTo({left + bl.x, bottom});
  if (bl.x > 0) {
    builder.curveTo({left + bl.x - bl.x * kKappa, bottom}, {left, bottom - bl.y + bl.y * kKappa},
                    {left, bottom - bl.y});
  }
  builder.lineTo({left, top + tl.y});
  if (tl.x > 0) {
    builder.curveTo({left, top + tl.y - tl.y * kKappa}, {left + tl.x - tl.x * kKappa, top},
                    {left + tl.x, top});
  }
  return builder.closePath().build();
}

Path BuildPolygonClipPath(const PolygonClipPath& polygon, const Box2d& box,
                          const FontMetrics& metrics) {
  PathBuilder builder(polygon.vertices.size());
  bool first = true;
  for (const auto& vertex : polygon.vertices) {
    const Vector2d point =
        box.topLeft + Vector2d(vertex.x.toPixels(box, metrics, Lengthd::Extent::X),
                               vertex.y.toPixels(box, metrics, Lengthd::Extent::Y));
    if (!IsRenderableClipCoordinate(point.x) || !IsRenderableClipCoordinate(point.y)) {
      return {};
    }
    if (first) {
      builder.moveTo(point);
      first = false;
    } else {
      builder.lineTo(point);
    }
  }
  Path path = builder.closePath().build();
  const Box2d bounds = path.bounds();
  if (!IsRenderableClipCoordinate(bounds.width()) || !IsRenderableClipCoordinate(bounds.height())) {
    return {};
  }
  return path;
}

bool PathClipCoordinatesFit(const Path& path, const Vector2d& origin) {
  if (path.points().empty()) {
    return false;
  }
  Box2d bounds = Box2d::CreateEmpty(path.points().front() + origin);
  for (const Vector2d& source : path.points()) {
    const Vector2d point = source + origin;
    if (!IsRenderableClipCoordinate(point.x) || !IsRenderableClipCoordinate(point.y)) {
      return false;
    }
    bounds.addPoint(point);
  }
  return IsRenderableClipCoordinate(bounds.width()) && IsRenderableClipCoordinate(bounds.height());
}

Path BuildClosedPathClip(const Path& path, const Vector2d& origin) {
  PathBuilder builder(path.points().size());
  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo: builder.closePath().moveTo(points[0] + origin); break;
      case Path::Verb::LineTo: builder.lineTo(points[0] + origin); break;
      case Path::Verb::QuadTo: builder.quadTo(points[0] + origin, points[1] + origin); break;
      case Path::Verb::CurveTo:
        builder.curveTo(points[0] + origin, points[1] + origin, points[2] + origin);
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  });
  return builder.closePath().build();
}

class ReferenceBoundsAccumulator {
public:
  ReferenceBoundsAccumulator(ShapeSystem& shapes, EntityHandle root,
                             const Transform2d& targetFromWorld, bool includeStroke,
                             CircleClipGeometryBudget* budget)
      : shapes_(shapes),
        root_(root),
        targetFromWorld_(targetFromWorld),
        includeStroke_(includeStroke),
        budget_(budget) {}

  bool visit(EntityHandle child) {
    if (failed_ || (budget_ && !budget_->reserve(1))) {
      failed_ = true;
      return false;
    }
    if (IsDisplayNone(child, warningSink_)) {
      return false;
    }

    const Transform2d worldFromChild = LayoutSystem().getEntityFromWorldTransform(child);
    accumulatePath(child, worldFromChild);
    if (failed_) {
      return false;
    }
    accumulateSizedElement(child, worldFromChild * targetFromWorld_);
    return !failed_ && !accumulateText(child) && !failed_;
  }

  std::optional<Box2d> result() const { return failed_ ? std::nullopt : bounds_; }

private:
  void add(const Box2d& bounds) {
    if (!IsFiniteBox(bounds)) {
      RejectGeometry(budget_, failed_);
      return;
    }
    bounds_ = bounds_ ? Box2d::Union(*bounds_, bounds) : bounds;
  }

  ComputedPathComponent* referencePath(EntityHandle child) {
    if (const auto* shadow = child.try_get<ShadowEntityComponent>()) {
      // Reference bounds do not need the renderer's cache-sharing identity. Selecting the
      // occurrence's prepared path directly avoids an unadmitted full-path equality scan.
      const Entity owner =
          child.all_of<ComputedInstanceGeometryComponent>() ? child.entity() : shadow->lightEntity;
      return child.registry()->try_get<ComputedPathComponent>(owner);
    }
    return shapes_.createComputedPathIfShape(child, FontMetrics(), warningSink_);
  }

  void accumulatePath(EntityHandle child, const Transform2d& worldFromChild) {
    ComputedPathComponent* path = referencePath(child);
    if (!path) {
      return;
    }
    // Even a fill-only reference box scans the entire path; admit that scan before it begins.
    if (!ReservePathGeometry(budget_, path->spline)) {
      RejectGeometry(budget_, failed_);
      return;
    }
    add(path->transformedBounds(worldFromChild * targetFromWorld_));
    if (!includeStroke_) {
      return;
    }
    if (const auto bounds = StrokeBounds(child, path->spline, worldFromChild, targetFromWorld_,
                                         budget_, false, failed_)) {
      add(*bounds);
    }
  }

  void accumulateSizedElement(EntityHandle child, const Transform2d& targetFromChild) {
    EntityHandle light = child;
    if (const auto* shadow = child.try_get<ShadowEntityComponent>()) {
      light = EntityHandle(*child.registry(), shadow->lightEntity);
    }
    const auto* type = light.try_get<ElementTypeComponent>();
    if (!type || type->type() != ElementType::Image) {
      return;
    }
    // Structural type and fallback dimensions live on the light element. Any instance sizing
    // wins, while the caller always supplies the occurrence's transform.
    if (const auto* sized = child.try_get<ComputedShadowSizedElementComponent>()) {
      add(targetFromChild.transformBox(sized->bounds));
    } else if (const auto* sized = child.try_get<ComputedSizedElementComponent>()) {
      add(targetFromChild.transformBox(sized->bounds));
    } else if (const auto* sized = light.try_get<ComputedSizedElementComponent>()) {
      add(targetFromChild.transformBox(sized->bounds));
    }
  }

  bool accumulateText(EntityHandle entity) {
#ifdef DONNER_TEXT_ENABLED
    auto* textEngine = entity.registry()->ctx().find<TextEngine>();
    if (!textEngine) {
      return false;
    }
    EntityHandle geometryEntity = entity;
    EntityHandle transformEntity = entity;
    const components::ComputedTextComponent* authoredText = nullptr;
    if (const auto* shadow = entity.try_get<ShadowEntityComponent>()) {
      geometryEntity = EntityHandle(*entity.registry(), shadow->lightEntity);
    }
    const Entity textRoot = textEngine->textRootEntity(geometryEntity);
    if (textRoot == entt::null ||
        (entity.entity() != root_.entity() && geometryEntity.entity() != textRoot)) {
      return false;
    }
    std::size_t localWork = CircleClipGeometryBudget::kMaximumWork;
    std::size_t& remainingWork = budget_ ? budget_->remainingWork : localWork;
    std::optional<TextStyleContext> styleContext;
    if (entity.all_of<ShadowEntityComponent>()) {
      authoredText = entity.registry()->try_get<components::ComputedTextComponent>(textRoot);
      if (!authoredText) {
        RejectGeometry(budget_, failed_);
        return true;
      }
      styleContext = BuildShadowTextStyleContext(*entity.registry(), entity.entity(), *authoredText,
                                                 &remainingWork);
      if (!styleContext) {
        RejectGeometry(budget_, failed_);
        return true;
      }
    } else {
      transformEntity = EntityHandle(*entity.registry(), textRoot);
    }
    accumulateTextGeometry(*textEngine, geometryEntity, transformEntity, std::move(styleContext),
                           remainingWork);
    return true;
#else
    (void)entity;
    return false;
#endif
  }

#ifdef DONNER_TEXT_ENABLED
  void accumulateTextGeometry(TextEngine& textEngine, EntityHandle geometryEntity,
                              EntityHandle transformEntity,
                              std::optional<TextStyleContext> styleContext,
                              std::size_t& remainingWork) {
    const Transform2d worldFromText = LayoutSystem().getEntityFromWorldTransform(transformEntity);
    if (!budget_ && !includeStroke_ && !styleContext) {
      // Preserve the existing public object-bounds query; circle reference queries always carry
      // a budget and must not enter this painted-cache construction path.
      const Box2d bounds = textEngine.computedObjectBoundingBox(geometryEntity);
      if (!bounds.isEmpty()) {
        add((worldFromText * targetFromWorld_).transformBox(bounds));
      }
      return;
    }
    const auto geometry = textEngine.referenceGeometry(
        geometryEntity, includeStroke_, remainingWork, styleContext ? &*styleContext : nullptr);
    if (!geometry) {
      RejectGeometry(budget_, failed_);
      return;
    }
    if (geometry->objectBounds) {
      add((worldFromText * targetFromWorld_).transformBox(*geometry->objectBounds));
    }
    for (const auto& glyph : geometry->outlines) {
      const EntityHandle source(*geometryEntity.registry(), glyph.sourceEntity);
      if (const auto bounds = StrokeBounds(source, glyph.path, worldFromText, targetFromWorld_,
                                           budget_, true, failed_)) {
        add(*bounds);
      }
      if (failed_) {
        return;
      }
    }
  }

#endif

  ShapeSystem& shapes_;
  EntityHandle root_;
  const Transform2d& targetFromWorld_;
  bool includeStroke_;
  CircleClipGeometryBudget* budget_;
  ParseWarningSink warningSink_ = ParseWarningSink::Disabled();
  std::optional<Box2d> bounds_;
  bool failed_ = false;
};

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
  const Transform2d outerEntityFromWorld =
      LayoutSystem().getEntityFromWorldTransform(handle).inverse();

  return getTransformedShapeBounds(handle, outerEntityFromWorld);
}

std::optional<Box2d> ShapeSystem::getStrokeBounds(EntityHandle handle) {
  const Transform2d outerEntityFromWorld =
      LayoutSystem().getEntityFromWorldTransform(handle).inverse();
  return getTransformedShapeBounds(handle, outerEntityFromWorld, true);
}

Path ShapeSystem::circleClipPath(EntityHandle handle, const CircleClipPath& circle,
                                 CircleClipGeometryBudget& budget) {
  if (budget.rejected) {
    return {};
  }
  const std::optional<Box2d> box = getCircleReferenceBox(handle, circle.box, budget);
  const auto* style = handle.try_get<ComputedStyleComponent>();
  if (!box || !style || !style->properties) {
    return {};
  }
  const FontMetrics metrics = fontMetricsForElement(*handle.registry(), *style, handle.entity());
  const double offsetX = circle.x.offset.toPixels(*box, metrics, Lengthd::Extent::X);
  const double offsetY = circle.y.offset.toPixels(*box, metrics, Lengthd::Extent::Y);
  const double x = circle.x.fromFarEdge ? box->width() - offsetX : offsetX;
  const double y = circle.y.fromFarEdge ? box->height() - offsetY : offsetY;
  const double radius = CircleRadius(circle, *box, x, y, metrics);
  const Vector2d center = box->topLeft + Vector2d(x, y);
  if (!IsFiniteCircle(center, radius)) {
    return {};
  }
  Path result = PathBuilder(32).addCircle(center, radius).build();
  if (!ReservePathGeometry(&budget, result)) {
    return {};
  }
  return result;
}

Path ShapeSystem::ellipseClipPath(EntityHandle handle, const EllipseClipPath& ellipse,
                                  CircleClipGeometryBudget& budget) {
  if (budget.rejected) {
    return {};
  }
  const std::optional<Box2d> box = getCircleReferenceBox(handle, ellipse.box, budget);
  const auto* style = handle.try_get<ComputedStyleComponent>();
  if (!box || !style || !style->properties) {
    return {};
  }
  const FontMetrics metrics = fontMetricsForElement(*handle.registry(), *style, handle.entity());
  const double offsetX = ellipse.x.offset.toPixels(*box, metrics, Lengthd::Extent::X);
  const double offsetY = ellipse.y.offset.toPixels(*box, metrics, Lengthd::Extent::Y);
  const double x = ellipse.x.fromFarEdge ? box->width() - offsetX : offsetX;
  const double y = ellipse.y.fromFarEdge ? box->height() - offsetY : offsetY;
  const double rx =
      EllipseRadius(ellipse.radiusXType, ellipse.radiusX, *box, metrics, Lengthd::Extent::X, x);
  const double ry =
      EllipseRadius(ellipse.radiusYType, ellipse.radiusY, *box, metrics, Lengthd::Extent::Y, y);
  const Vector2d center = box->topLeft + Vector2d(x, y);
  if (!IsFiniteEllipse(center, rx, ry)) {
    return {};
  }
  Path result = PathBuilder(32)
                    .addEllipse(Box2d(center - Vector2d(rx, ry), center + Vector2d(rx, ry)))
                    .build();
  if (!ReservePathGeometry(&budget, result)) {
    return {};
  }
  return result;
}

Path ShapeSystem::polygonClipPath(EntityHandle handle, const PolygonClipPath& polygon,
                                  CircleClipGeometryBudget& budget) {
  const std::size_t count = polygon.vertices.size();
  if (count > PolygonClipPath::kMaximumVertices || !budget.reserve(count * 2 + 1)) {
    return {};
  }
  if (count < 3) {
    return {};
  }
  const std::optional<Box2d> box = getCircleReferenceBox(handle, polygon.box, budget);
  const auto* style = handle.try_get<ComputedStyleComponent>();
  if (!box || !style || !style->properties) {
    return {};
  }
  const FontMetrics metrics = fontMetricsForElement(*handle.registry(), *style, handle.entity());
  return BuildPolygonClipPath(polygon, *box, metrics);
}

Path ShapeSystem::insetClipPath(EntityHandle handle, const InsetClipPath& inset,
                                CircleClipGeometryBudget& budget) {
  if (!budget.reserve(inset.hasRoundedCorners() ? 27 : 9)) {
    return {};
  }
  const std::optional<Box2d> box = getCircleReferenceBox(handle, inset.box, budget);
  const auto* style = handle.try_get<ComputedStyleComponent>();
  if (!box || !style || !style->properties) {
    return {};
  }
  const FontMetrics metrics = fontMetricsForElement(*handle.registry(), *style, handle.entity());
  const double top = box->topLeft.y + inset.offsets[0].toPixels(*box, metrics, Lengthd::Extent::Y);
  const double right =
      box->bottomRight.x - inset.offsets[1].toPixels(*box, metrics, Lengthd::Extent::X);
  const double bottom =
      box->bottomRight.y - inset.offsets[2].toPixels(*box, metrics, Lengthd::Extent::Y);
  const double left = box->topLeft.x + inset.offsets[3].toPixels(*box, metrics, Lengthd::Extent::X);
  const std::array<double, 6> coordinates = {left, top, right, bottom, right - left, bottom - top};
  if (!std::all_of(coordinates.begin(), coordinates.end(), IsRenderableClipCoordinate)) {
    return {};
  }
  // Proportional reduction of oversized opposing insets has zero area for clipping.
  if (right <= left || bottom <= top) {
    return {};
  }
  const Box2d rect({left, top}, {right, bottom});
  const auto radii = ResolveInsetRadii(inset, *box, metrics, rect.size());
  if (!radii) {
    return {};
  }
  return BuildRoundedInsetClip(rect, *radii);
}

Path ShapeSystem::pathClipPath(EntityHandle handle, const PathClipPath& path,
                               CircleClipGeometryBudget& budget) {
  const std::size_t points = path.data.points().size();
  const std::size_t commands = path.data.commands().size();
  if (points > parser::PathParser::kMaximumPoints ||
      commands > parser::PathParser::kMaximumCommands) {
    budget.reject();
    return {};
  }
  // Charge validation, translated copies and an implicit close for every possible contour.
  if (!budget.reserve(2 * points + 2 * commands + 1)) {
    return {};
  }
  const std::optional<Box2d> box = getCircleReferenceBox(handle, path.box, budget);
  if (!box || !PathClipCoordinatesFit(path.data, box->topLeft)) {
    return {};
  }
  return BuildClosedPathClip(path.data, box->topLeft);
}

std::optional<Box2d> ShapeSystem::getCircleReferenceBox(EntityHandle handle,
                                                        CircleClipPath::Box box,
                                                        CircleClipGeometryBudget& budget) {
  const Transform2d localFromWorld = LayoutSystem().getEntityFromWorldTransform(handle).inverse();
  switch (box) {
    case CircleClipPath::Box::Fill:
      return getTransformedShapeBounds(handle, localFromWorld, false, &budget);
    case CircleClipPath::Box::Stroke:
      return getTransformedShapeBounds(handle, localFromWorld, true, &budget);
    case CircleClipPath::Box::View: return LayoutSystem().getViewBox(handle);
  }
  return std::nullopt;
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
                                                            const Transform2d& targetFromWorld,
                                                            bool includeStroke,
                                                            CircleClipGeometryBudget* budget) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  if (IsDisplayNone(handle, disabledSink)) {
    return std::nullopt;
  }
  const ScopedTextPathBoundsWorkEpoch textPathWorkEpoch(*handle.registry());
  ReferenceBoundsAccumulator accumulator(*this, handle, targetFromWorld, includeStroke, budget);
  donner::components::ForAllChildrenRecursivePruned(
      handle, [&accumulator](EntityHandle child) { return accumulator.visit(child); });
  return accumulator.result();
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
