#include "donner/svg/renderer/RendererDriver.h"

#include <any>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/base/ParseError.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/core/Overflow.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

namespace {

PathShape toPathShape(const components::ComputedPathComponent& path,
                      const components::ComputedStyleComponent& style) {
  PathShape shape;
  shape.path = path.spline;
  shape.fillRule = style.properties->fillRule.getRequired();
  return shape;
}

StrokeParams toStrokeParams(Registry& registry,
                            const components::RenderingInstanceComponent& instance,
                            const components::ComputedStyleComponent& style) {
  StrokeParams stroke;
  const auto& properties = style.properties.value();

  const Boxd viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
  const FontMetrics baseFontMetrics = FontMetrics::DefaultsWithFontSize(16.0);
  const double fontSizePx = properties.fontSize.getRequired().toPixels(viewBox, baseFontMetrics);
  const FontMetrics fontMetrics = FontMetrics::DefaultsWithFontSize(fontSizePx);

  const auto toPixels = [&](const Lengthd& length,
                            Lengthd::Extent extent = Lengthd::Extent::Mixed) {
    return length.toPixels(viewBox, fontMetrics, extent);
  };

  stroke.strokeWidth = toPixels(properties.strokeWidth.getRequired());
  stroke.lineCap = properties.strokeLinecap.getRequired();
  stroke.lineJoin = properties.strokeLinejoin.getRequired();
  stroke.miterLimit = properties.strokeMiterlimit.getRequired();
  stroke.dashOffset = toPixels(properties.strokeDashoffset.getRequired());

  if (const std::optional<StrokeDasharray> dashArray = properties.strokeDasharray.get()) {
    stroke.dashArray.reserve(dashArray->size());
    for (const Lengthd& dashLength : *dashArray) {
      stroke.dashArray.push_back(toPixels(dashLength));
    }
  }

  if (const auto* pathLengthComp =
          instance.dataHandle(registry).try_get<components::PathLengthComponent>()) {
    stroke.pathLength = pathLengthComp->value;
  }

  return stroke;
}

PaintParams toPaintParams(Registry& registry,
                          const components::RenderingInstanceComponent& instance,
                          const components::ComputedStyleComponent& style) {
  PaintParams paint;
  const auto& properties = style.properties.value();

  paint.opacity = properties.opacity.getRequired();
  paint.fill = instance.resolvedFill;
  paint.fillOpacity = properties.fillOpacity.getRequired();
  paint.stroke = instance.resolvedStroke;
  paint.strokeOpacity = properties.strokeOpacity.getRequired();
  paint.currentColor = properties.color.getRequired();
  paint.viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
  paint.strokeParams = toStrokeParams(registry, instance, style);

  return paint;
}

ResolvedClip toResolvedClip(const components::RenderingInstanceComponent& instance,
                            const components::ComputedStyleComponent& style, Registry& registry) {
  ResolvedClip clip;
  clip.clipRect = instance.clipRect;
  // Note: mask is NOT copied here — it's extracted separately by the caller via
  // entityClip.mask before being moved out. This avoids copying the non-copyable
  // ResolvedMask (which contains std::unique_ptr<ResolvedMask> parentMask chain).

  if (const auto* clipPaths =
          instance.styleHandle(registry).try_get<components::ComputedClipPathsComponent>()) {
    // Compute objectBoundingBox transform if needed.
    if (instance.clipPath && instance.clipPath->units == ClipPathUnits::ObjectBoundingBox) {
      if (auto maybeBounds =
              components::ShapeSystem().getShapeBounds(instance.dataHandle(registry))) {
        const Boxd bounds = maybeBounds.value();
        clip.clipPathUnitsTransform =
            Transformd::Scale(bounds.size()) * Transformd::Translate(bounds.topLeft);
      }
    }

    clip.clipPaths.reserve(clipPaths->clipPaths.size());
    for (const auto& path : clipPaths->clipPaths) {
      PathShape shape;
      shape.path = path.path;
      shape.fillRule = path.clipRule == ClipRule::NonZero ? FillRule::NonZero : FillRule::EvenOdd;
      shape.entityFromParent = path.entityFromParent;
      shape.layer = path.layer;
      clip.clipPaths.push_back(shape);
    }
  }

  // If clip-path is set but has no computed paths (e.g., empty <clipPath> element),
  // add an empty path to force a clip that hides everything.
  if (instance.clipPath.has_value() && clip.clipPaths.empty()) {
    clip.clipPaths.emplace_back();
  }

  // The computed clip rule is inherited if undefined on the clip path itself.
  for (PathShape& path : clip.clipPaths) {
    if (path.fillRule == FillRule::NonZero &&
        style.properties->clipRule.getRequired() == ClipRule::EvenOdd) {
      path.fillRule = FillRule::EvenOdd;
    }
  }

  return clip;
}

/// Convert an absolute Lengthd to pixel value. For relative units (em, %, etc.) falls back
/// to the raw value.
double lengthToPixels(const Lengthd& length) {
  switch (length.unit) {
    case Lengthd::Unit::None:
    case Lengthd::Unit::Px: return length.value;
    case Lengthd::Unit::In: return length.value * 96.0;
    case Lengthd::Unit::Cm: return length.value * 96.0 / 2.54;
    case Lengthd::Unit::Mm: return length.value * 96.0 / 25.4;
    case Lengthd::Unit::Q: return length.value * 96.0 / (2.54 * 40.0);
    case Lengthd::Unit::Pt: return length.value * 96.0 / 72.0;
    case Lengthd::Unit::Pc: return length.value * 96.0 / 6.0;
    default: return length.value;  // Relative units: best-effort fallback.
  }
}

std::optional<components::FilterGraph> resolveFilterGraph(
    Registry& registry, const components::ResolvedFilterEffect& filter,
    css::RGBA currentColor = css::RGBA(0, 0, 0, 0xFF)) {
  if (const auto* effects = std::get_if<std::vector<FilterEffect>>(&filter)) {
    // Convert CSS filter functions to a FilterGraph.
    // Per CSS Filter Effects Level 1, CSS filter shorthand functions use sRGB color space.
    components::FilterGraph graph;
    graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
    for (const FilterEffect& effect : *effects) {
      std::visit(
          [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, FilterEffect::None>) {
              // No-op.
            } else if constexpr (std::is_same_v<T, FilterEffect::Blur>) {
              components::FilterNode node;
              node.primitive = components::filter_primitive::GaussianBlur{
                  .stdDeviationX = lengthToPixels(e.stdDeviationX),
                  .stdDeviationY = lengthToPixels(e.stdDeviationY),
              };
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::ElementReference>) {
              // Resolve the element reference and copy its filter graph nodes.
              // Preserve the source filter's color space on each copied node so that
              // url() nodes run in the <filter> element's color space (typically linearRGB)
              // while CSS function nodes use the graph-level sRGB.
              if (auto resolvedRef = e.reference.resolve(registry);
                  resolvedRef && resolvedRef->handle.template all_of<
                                     components::ComputedFilterComponent>()) {
                if (const auto* computed =
                        registry.try_get<components::ComputedFilterComponent>(*resolvedRef)) {
                  const auto srcColorSpace = computed->filterGraph.colorInterpolationFilters;
                  for (auto refNode : computed->filterGraph.nodes) {
                    if (!refNode.colorInterpolationFilters.has_value()) {
                      refNode.colorInterpolationFilters = srcColorSpace;
                    }
                    graph.nodes.push_back(std::move(refNode));
                  }
                }
              }

            } else if constexpr (std::is_same_v<T, FilterEffect::HueRotate>) {
              components::FilterNode node;
              node.primitive = components::filter_primitive::ColorMatrix{
                  .type = components::filter_primitive::ColorMatrix::Type::HueRotate,
                  .values = {e.angleDegrees},
              };
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::Brightness>) {
              components::FilterNode node;
              components::filter_primitive::ComponentTransfer ct;
              ct.funcR = {.type = components::filter_primitive::ComponentTransfer::FuncType::Linear,
                          .slope = e.amount,
                          .intercept = 0.0};
              ct.funcG = ct.funcR;
              ct.funcB = ct.funcR;
              node.primitive = std::move(ct);
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::Contrast>) {
              components::FilterNode node;
              components::filter_primitive::ComponentTransfer ct;
              ct.funcR = {.type = components::filter_primitive::ComponentTransfer::FuncType::Linear,
                          .slope = e.amount,
                          .intercept = -(0.5 * e.amount) + 0.5};
              ct.funcG = ct.funcR;
              ct.funcB = ct.funcR;
              node.primitive = std::move(ct);
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::Grayscale>) {
              // grayscale(n) = saturate(1 - n)
              components::FilterNode node;
              node.primitive = components::filter_primitive::ColorMatrix{
                  .type = components::filter_primitive::ColorMatrix::Type::Saturate,
                  .values = {1.0 - e.amount},
              };
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::Invert>) {
              components::FilterNode node;
              components::filter_primitive::ComponentTransfer ct;
              ct.funcR = {
                  .type = components::filter_primitive::ComponentTransfer::FuncType::Table,
                  .tableValues = {e.amount, 1.0 - e.amount},
              };
              ct.funcG = ct.funcR;
              ct.funcB = ct.funcR;
              node.primitive = std::move(ct);
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::FilterOpacity>) {
              components::FilterNode node;
              components::filter_primitive::ComponentTransfer ct;
              ct.funcA = {.type = components::filter_primitive::ComponentTransfer::FuncType::Linear,
                          .slope = e.amount,
                          .intercept = 0.0};
              node.primitive = std::move(ct);
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::Saturate>) {
              components::FilterNode node;
              node.primitive = components::filter_primitive::ColorMatrix{
                  .type = components::filter_primitive::ColorMatrix::Type::Saturate,
                  .values = {e.amount},
              };
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::Sepia>) {
              // Sepia matrix per CSS Filter Effects Level 1 spec.
              const double s = e.amount;
              components::FilterNode node;
              // clang-format off
              node.primitive = components::filter_primitive::ColorMatrix{
                  .type = components::filter_primitive::ColorMatrix::Type::Matrix,
                  .values = {
                      0.393 + 0.607 * (1.0 - s), 0.769 - 0.769 * (1.0 - s), 0.189 - 0.189 * (1.0 - s), 0, 0,
                      0.349 - 0.349 * (1.0 - s), 0.686 + 0.314 * (1.0 - s), 0.168 - 0.168 * (1.0 - s), 0, 0,
                      0.272 - 0.272 * (1.0 - s), 0.534 - 0.534 * (1.0 - s), 0.131 + 0.869 * (1.0 - s), 0, 0,
                      0, 0, 0, 1, 0,
                  },
              };
              // clang-format on
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));

            } else if constexpr (std::is_same_v<T, FilterEffect::DropShadow>) {
              // Resolve currentColor to the element's computed color property value.
              const css::RGBA resolvedRGBA = e.color.resolve(currentColor, 1.0f);
              components::FilterNode node;
              node.primitive = components::filter_primitive::DropShadow{
                  .dx = lengthToPixels(e.offsetX),
                  .dy = lengthToPixels(e.offsetY),
                  .stdDeviationX = lengthToPixels(e.stdDeviation),
                  .stdDeviationY = lengthToPixels(e.stdDeviation),
                  .floodColor = css::Color(resolvedRGBA),
                  .floodOpacity = static_cast<double>(resolvedRGBA.a) / 255.0,
              };
              node.inputs.push_back(components::FilterInput{});
              graph.nodes.push_back(std::move(node));
            }
          },
          effect.value);
    }
    return graph;
  }

  if (const auto* reference = std::get_if<ResolvedReference>(&filter)) {
    if (const auto* computed =
            registry.try_get<components::ComputedFilterComponent>(*reference)) {
      return computed->filterGraph;
    }
  }

  return std::nullopt;
}

/// Compute the filter region bounds in entity-local coordinates from a resolved filter reference.
std::optional<Boxd> computeFilterRegion(
    Registry& registry, const components::ResolvedFilterEffect& filter,
    const components::RenderingInstanceComponent& instance) {
  const auto* reference = std::get_if<ResolvedReference>(&filter);
  if (!reference) {
    // For CSS filter function lists, check if any entry is a url() reference.
    // url() references need a filter region; pure CSS functions don't.
    const auto* effects = std::get_if<std::vector<FilterEffect>>(&filter);
    if (!effects) {
      return std::nullopt;
    }

    const bool hasUrlReference = std::any_of(effects->begin(), effects->end(), [](const auto& e) {
      return e.template is<FilterEffect::ElementReference>();
    });
    if (!hasUrlReference) {
      return std::nullopt;  // Pure CSS filter functions: no filter region clipping.
    }

    // Mixed list with url() references: use the default filter region.
    const std::optional<Boxd> shapeBounds =
        components::ShapeSystem().getShapeBounds(instance.dataHandle(registry));
    if (!shapeBounds) {
      return std::nullopt;
    }
    const double bx = shapeBounds->topLeft.x;
    const double by = shapeBounds->topLeft.y;
    const double bw = shapeBounds->width();
    const double bh = shapeBounds->height();
    return Boxd::FromXYWH(bx - 0.1 * bw, by - 0.1 * bh, 1.2 * bw, 1.2 * bh);
  }

  const auto* computed = registry.try_get<components::ComputedFilterComponent>(*reference);
  if (!computed) {
    return std::nullopt;
  }

  // Determine the bounds reference for resolving percentages.
  const Boxd shapeBounds =
      components::ShapeSystem().getShapeBounds(instance.dataHandle(registry)).value_or(Boxd());

  if (computed->filterUnits == FilterUnits::ObjectBoundingBox) {
    // In objectBoundingBox mode, x/y/width/height are fractions/percentages of the bbox.
    // Unitless values are fractions (e.g., 0.1 = 10% of bbox dimension).
    // Percent values use toPixels which handles the /100 division.
    auto resolveOBB = [&](const Lengthd& len, double bboxDim) -> double {
      if (len.unit == Lengthd::Unit::None) {
        return len.value * bboxDim;
      }
      // Percent and other units: toPixels resolves relative to shapeBounds.
      return len.toPixels(shapeBounds, FontMetrics(),
                          bboxDim == shapeBounds.width() ? Lengthd::Extent::X : Lengthd::Extent::Y);
    };
    const double xPx = resolveOBB(computed->x, shapeBounds.width()) + shapeBounds.topLeft.x;
    const double yPx = resolveOBB(computed->y, shapeBounds.height()) + shapeBounds.topLeft.y;
    const double wPx = resolveOBB(computed->width, shapeBounds.width());
    const double hPx = resolveOBB(computed->height, shapeBounds.height());
    return Boxd::FromXYWH(xPx, yPx, wPx, hPx);
  }

  const Boxd viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
  const double xPx = computed->x.toPixels(viewBox, FontMetrics(), Lengthd::Extent::X);
  const double yPx = computed->y.toPixels(viewBox, FontMetrics(), Lengthd::Extent::Y);
  const double wPx = computed->width.toPixels(viewBox, FontMetrics(), Lengthd::Extent::X);
  const double hPx = computed->height.toPixels(viewBox, FontMetrics(), Lengthd::Extent::Y);
  return Boxd::FromXYWH(xPx, yPx, wPx, hPx);
}

css::Color resolveFillColor(const components::RenderingInstanceComponent& instance,
                            const components::ComputedStyleComponent& style) {
  const auto& properties = style.properties.value();
  const css::RGBA currentColor = properties.color.getRequired().rgba();
  const float fillOpacity = NarrowToFloat(properties.fillOpacity.getRequired());

  const auto resolveSolid = [&](const PaintServer::Solid& solid) {
    return css::Color(solid.color.resolve(currentColor, fillOpacity));
  };

  if (const auto* resolved = std::get_if<PaintServer::Solid>(&instance.resolvedFill)) {
    return resolveSolid(*resolved);
  }

  const PaintServer fill = properties.fill.getRequired();
  if (const auto* solid = std::get_if<PaintServer::Solid>(&fill.value)) {
    return resolveSolid(*solid);
  }

  return css::Color(css::RGBA(currentColor.r, currentColor.g, currentColor.b,
                              static_cast<uint8_t>(NarrowToFloat(currentColor.a) * fillOpacity)));
}

TextParams toTextParams(Registry& registry, const components::RenderingInstanceComponent& instance,
                        const components::ComputedStyleComponent& style,
                        const components::TextComponent* textComp) {
  TextParams params;
  const auto& properties = style.properties.value();
  const css::RGBA currentColor = properties.color.getRequired().rgba();

  params.opacity = properties.opacity.getRequired();
  params.fillColor = resolveFillColor(instance, style);

  if (const auto* stroke = std::get_if<PaintServer::Solid>(&instance.resolvedStroke)) {
    const float strokeOpacity = NarrowToFloat(properties.strokeOpacity.getRequired());
    params.strokeColor = css::Color(stroke->color.resolve(currentColor, strokeOpacity));
    params.strokeParams = toStrokeParams(registry, instance, style);
  }

  params.fontFamilies = properties.fontFamily.getRequired();
  params.fontSize = properties.fontSize.getRequired();
  params.viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
  params.fontMetrics = FontMetrics();
  params.textAnchor = properties.textAnchor.getRequired();
  params.textDecoration = properties.textDecoration.getRequired();
  params.dominantBaseline = properties.dominantBaseline.getRequired();
  params.writingMode = properties.writingMode.getRequired();

  // Resolve letter-spacing and word-spacing to pixels.
  params.letterSpacingPx = properties.letterSpacing.getRequired().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.wordSpacingPx = properties.wordSpacing.getRequired().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);

  if (textComp) {
    params.textLength = textComp->textLength;
    params.lengthAdjust = textComp->lengthAdjust;
  }

  // Pass @font-face declarations so renderers can resolve custom fonts.
  if (auto* resourceManager = registry.ctx().find<components::ResourceManagerContext>()) {
    params.fontFaces = resourceManager->fontFaces();
  }

  return params;
}

std::optional<ImageParams> toImageParams(const components::RenderingInstanceComponent& instance,
                                         const components::ComputedStyleComponent& style,
                                         const components::LoadedImageComponent& image,
                                         Registry& registry) {
  if (!image.image.has_value()) {
    return std::nullopt;
  }

  const auto* sizedElement =
      instance.dataHandle(registry).try_get<components::ComputedSizedElementComponent>();
  if (sizedElement == nullptr) {
    return std::nullopt;
  }

  ImageParams params;
  params.opacity = style.properties->opacity.getRequired();
  params.targetRect = Boxd::WithSize(Vector2d(image.image->width, image.image->height));

  return params;
}

}  // namespace

RendererDriver::RendererDriver(RendererInterface& renderer, bool verbose)
    : renderer_(renderer), verbose_(verbose) {}

void RendererDriver::draw(SVGDocument& document) {
  std::vector<ParseError> warnings;
  RendererUtils::prepareDocumentForRendering(document, verbose_, verbose_ ? &warnings : nullptr);

  if (!warnings.empty()) {
    for (const ParseError& warning : warnings) {
      std::cerr << warning << '\n';
    }
  }

  renderingSize_ = document.canvasSize();
  RenderViewport viewport;
  viewport.size = Vector2d(renderingSize_.x, renderingSize_.y);
  viewport.devicePixelRatio = 1.0;

  renderer_.beginFrame(viewport);
  RenderingInstanceView view(document.registry());
  traverse(view, document.registry());
  renderer_.endFrame();
}

void RendererDriver::drawEntityRange(Registry& registry, Entity firstEntity, Entity lastEntity,
                                     const RenderViewport& viewport,
                                     const Transformd& baseTransform) {
  renderingSize_ = Vector2i(static_cast<int>(viewport.size.x), static_cast<int>(viewport.size.y));
  layerBaseTransform_ = baseTransform;

  renderer_.beginFrame(viewport);

  RenderingInstanceView view(registry);

  // Advance to the first entity.
  while (!view.done() && view.currentEntity() != firstEntity) {
    view.advance();
  }

  // Traverse from first to last (inclusive).
  bool reachedLast = false;
  while (!view.done() && !reachedLast) {
    reachedLast = (view.currentEntity() == lastEntity);

    const components::RenderingInstanceComponent& instance = view.get();
    const Entity entity = view.currentEntity();
    view.advance();

    const auto& style = instance.styleHandle(registry).get<components::ComputedStyleComponent>();
    if (!style.properties.has_value()) {
      continue;
    }

    const bool hasViewportClip = instance.clipRect.has_value();
    if (hasViewportClip) {
      ResolvedClip viewportClip;
      viewportClip.clipRect = instance.clipRect;
      renderer_.pushClip(viewportClip);
    }

    renderer_.setTransform(layerBaseTransform_ * instance.entityFromWorldTransform);

    const double opacity = style.properties->opacity.getRequired();
    const MixBlendMode blendMode = style.properties->mixBlendMode.getRequired();
    const Isolation isolation = style.properties->isolation.getRequired();
    const bool hasIsolatedLayer =
        opacity < 1.0 || blendMode != MixBlendMode::Normal || isolation == Isolation::Isolate;
    if (hasIsolatedLayer) {
      renderer_.pushIsolatedLayer(opacity, blendMode);
    }

    std::optional<components::FilterGraph> filterGraph =
        instance.resolvedFilter.has_value()
            ? resolveFilterGraph(registry, instance.resolvedFilter.value(),
                                 style.properties->color.getRequired().rgba())
            : std::nullopt;
    const std::optional<Boxd> filterRegion =
        instance.resolvedFilter.has_value()
            ? computeFilterRegion(registry, instance.resolvedFilter.value(), instance)
            : std::nullopt;
    if (filterGraph.has_value()) {
      filterGraph->filterRegion = filterRegion;
      if (filterGraph->primitiveUnits == PrimitiveUnits::ObjectBoundingBox) {
        filterGraph->elementBoundingBox =
            components::ShapeSystem().getShapeBounds(instance.dataHandle(registry));
      }
      const Boxd viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
      if (viewBox.width() > 0 && viewBox.height() > 0) {
        filterGraph->userToPixelScale =
            Vector2d(renderingSize_.x / viewBox.width(), renderingSize_.y / viewBox.height());
      }
    }
    const bool hasFilterLayer = filterGraph.has_value() && !filterGraph->empty();
    const bool filterHidesElement =
        instance.resolvedFilter.has_value() && !hasFilterLayer;

    ResolvedClip entityClip = toResolvedClip(instance, style, registry);
    entityClip.clipRect = std::nullopt;
    // Mask is handled separately from clip — access it directly from instance.
    const bool hasEntityClip = !entityClip.empty();
    if (hasEntityClip) {
      renderer_.pushClip(entityClip);
    }

    if (hasFilterLayer) {
      preRenderSvgFeImages(*filterGraph);
      preRenderFeImageFragments(*filterGraph, registry);
      renderer_.pushFilterLayer(*filterGraph, filterRegion);
    }

    int maskDepth = 0;
    bool subtreeConsumedBySubRendering = false;

    if (instance.mask.has_value() && instance.mask->valid()) {
      maskDepth = renderMask(view, registry, instance, *instance.mask);
      subtreeConsumedBySubRendering = true;
    }

    if (const auto* fillRef =
            std::get_if<components::PaintResolvedReference>(&instance.resolvedFill)) {
      if (fillRef->subtreeInfo &&
          fillRef->reference.handle.try_get<components::ComputedPatternComponent>()) {
        renderPattern(view, registry, instance, *fillRef, /*forStroke=*/false);
        subtreeConsumedBySubRendering = true;
      }
    }
    if (const auto* strokeRef =
            std::get_if<components::PaintResolvedReference>(&instance.resolvedStroke)) {
      if (strokeRef->subtreeInfo &&
          strokeRef->reference.handle.try_get<components::ComputedPatternComponent>()) {
        renderPattern(view, registry, instance, *strokeRef, /*forStroke=*/true);
        subtreeConsumedBySubRendering = true;
      }
    }

    const PaintParams paint = toPaintParams(registry, instance, style);
    renderer_.setPaint(paint);

    if (instance.visible && !filterHidesElement) {
      if (const auto* path =
              instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
        renderer_.drawPath(toPathShape(*path, style), paint.strokeParams);
        drawMarkers(view, registry, instance, *path, style);
      } else if (const auto* text =
                     instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
        const auto* textComp =
            instance.dataHandle(registry).try_get<components::TextComponent>();
        const TextParams textParams = toTextParams(registry, instance, style, textComp);
        renderer_.drawText(*text, textParams);
      } else if (const auto* image =
                     instance.dataHandle(registry).try_get<components::LoadedImageComponent>()) {
        const std::optional<ImageParams> imageParams =
            toImageParams(instance, style, *image, registry);
        if (imageParams.has_value()) {
          renderer_.drawImage(*image->image, *imageParams);
        }
      }
    }

    const bool subtreeConsumed = instance.subtreeInfo && subtreeConsumedBySubRendering;
    const bool shouldDefer = instance.subtreeInfo && !subtreeConsumed;
    if (shouldDefer) {
      DeferredPop deferred;
      deferred.lastEntity = instance.subtreeInfo->lastRenderedEntity;
      deferred.hasViewportClip = hasViewportClip;
      deferred.hasIsolatedLayer = hasIsolatedLayer;
      deferred.hasFilterLayer = hasFilterLayer;
      deferred.hasEntityClip = hasEntityClip;
      deferred.maskDepth = maskDepth;
      subtreeMarkers_.push_back(deferred);
    } else {
      for (int mi = 0; mi < maskDepth; ++mi) {
        renderer_.popMask();
      }
      if (hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (hasEntityClip) {
        renderer_.popClip();
      }
      if (hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
      if (hasViewportClip) {
        renderer_.popClip();
      }
    }

    while (!subtreeMarkers_.empty() && subtreeMarkers_.back().lastEntity == entity) {
      const DeferredPop& deferred = subtreeMarkers_.back();
      for (int mi = 0; mi < deferred.maskDepth; ++mi) {
        renderer_.popMask();
      }
      if (deferred.hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (deferred.hasEntityClip) {
        renderer_.popClip();
      }
      if (deferred.hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
      if (deferred.hasViewportClip) {
        renderer_.popClip();
      }
      subtreeMarkers_.pop_back();
    }
  }

  // Pop any remaining deferred layers (handles the case where lastEntity is
  // a subtree root whose deferred pop hasn't fired yet).
  while (!subtreeMarkers_.empty()) {
    const DeferredPop& deferred = subtreeMarkers_.back();
    for (int mi = 0; mi < deferred.maskDepth; ++mi) {
      renderer_.popMask();
    }
    if (deferred.hasFilterLayer) {
      renderer_.popFilterLayer();
    }
    if (deferred.hasEntityClip) {
      renderer_.popClip();
    }
    if (deferred.hasIsolatedLayer) {
      renderer_.popIsolatedLayer();
    }
    if (deferred.hasViewportClip) {
      renderer_.popClip();
    }
    subtreeMarkers_.pop_back();
  }

  renderer_.endFrame();
  layerBaseTransform_ = Transformd();
}

RendererBitmap RendererDriver::takeSnapshot() const {
  return renderer_.takeSnapshot();
}

void RendererDriver::traverse(RenderingInstanceView& view, Registry& registry) {
  while (!view.done()) {
    const components::RenderingInstanceComponent& instance = view.get();
    const Entity entity = view.currentEntity();
    view.advance();

    const auto& style = instance.styleHandle(registry).get<components::ComputedStyleComponent>();
    if (!style.properties.has_value()) {
      continue;
    }

    // Viewport clip rect is in the parent's coordinate space (includes x,y positioning).
    // Apply before the entity's own setTransform, so the clip is established with whatever
    // matrix was left from the previous element (which shares the parent's transform).
    const bool hasViewportClip = instance.clipRect.has_value();
    if (hasViewportClip) {
      ResolvedClip viewportClip;
      viewportClip.clipRect = instance.clipRect;
      renderer_.pushClip(viewportClip);
    }

    // Set the absolute transform for this entity. This uses setMatrix (no save/restore),
    // so it doesn't interact with the clip/layer save stack.
    // layerBaseTransform_ is composed with the entity's transform to support sub-document
    // rendering where a base transform maps from the parent document's coordinate space.
    if (verbose_) {
      const Transformd combined = layerBaseTransform_ * instance.entityFromWorldTransform;
      std::cout << "[traverse] entity=" << entt::to_integral(entity)
                << " visible=" << instance.visible
                << " maskDepth=" << instance.mask.has_value()
                << " hasSubtree=" << instance.subtreeInfo.has_value()
                << " transform=" << combined << "\n";
    }
    renderer_.setTransform(layerBaseTransform_ * instance.entityFromWorldTransform);

    const double opacity = style.properties->opacity.getRequired();
    const MixBlendMode blendMode = style.properties->mixBlendMode.getRequired();
    const Isolation isolation = style.properties->isolation.getRequired();
    const bool hasIsolatedLayer =
        opacity < 1.0 || blendMode != MixBlendMode::Normal || isolation == Isolation::Isolate;
    if (hasIsolatedLayer) {
      renderer_.pushIsolatedLayer(opacity, blendMode);
    }

    std::optional<components::FilterGraph> filterGraph =
        instance.resolvedFilter.has_value()
            ? resolveFilterGraph(registry, instance.resolvedFilter.value(),
                                 style.properties->color.getRequired().rgba())
            : std::nullopt;
    const std::optional<Boxd> filterRegion =
        instance.resolvedFilter.has_value()
            ? computeFilterRegion(registry, instance.resolvedFilter.value(), instance)
            : std::nullopt;
    if (filterGraph.has_value()) {
      filterGraph->filterRegion = filterRegion;
      if (filterGraph->primitiveUnits == PrimitiveUnits::ObjectBoundingBox) {
        filterGraph->elementBoundingBox =
            components::ShapeSystem().getShapeBounds(instance.dataHandle(registry));
      }
      // Compute the user-space to pixel-space scale factor from the viewBox and canvas dimensions.
      // Lighting filters need this to transform light positions from SVG attribute values to the
      // pixel-space pixmap coordinates.
      const Boxd viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
      if (viewBox.width() > 0 && viewBox.height() > 0) {
        filterGraph->userToPixelScale =
            Vector2d(renderingSize_.x / viewBox.width(), renderingSize_.y / viewBox.height());
      }
    }
    const bool hasFilterLayer = filterGraph.has_value() && !filterGraph->empty();
    // Per SVG spec, an empty or invalid filter reference makes the element invisible.
    const bool filterHidesElement =
        instance.resolvedFilter.has_value() && !hasFilterLayer;

    // Clip paths are in entity-local coordinates.
    // Per SVG spec, the rendering order is: paint → filter → clip-path → mask → opacity.
    // Push entity clip BEFORE filter so it's the outer layer: clip-path clips the filter output,
    // not the SourceGraphic input. The filter layer saves/clears the clip mask so the
    // SourceGraphic is rendered unclipped.
    ResolvedClip entityClip = toResolvedClip(instance, style, registry);
    entityClip.clipRect = std::nullopt;  // Already handled above as viewport clip.
    // Mask is handled separately below; don't let pushClip see it.
    // Mask is handled separately from clip — access it directly from instance.
    const bool hasEntityClip = !entityClip.empty();
    if (hasEntityClip) {
      renderer_.pushClip(entityClip);
    }

    if (hasFilterLayer) {
      preRenderSvgFeImages(*filterGraph);
      preRenderFeImageFragments(*filterGraph, registry);
      renderer_.pushFilterLayer(*filterGraph, filterRegion);
    }

    // Render mask content, then transition to masked content layer.
    int maskDepth = 0;
    // Track whether mask/pattern rendering consumed the element's subtree entities.
    bool subtreeConsumedBySubRendering = false;

    if (instance.mask.has_value() && instance.mask->valid()) {
      maskDepth = renderMask(view, registry, instance, *instance.mask);
      subtreeConsumedBySubRendering = true;
    }

    // Render pattern subtrees before drawing so the pattern shader is available.
    if (const auto* fillRef =
            std::get_if<components::PaintResolvedReference>(&instance.resolvedFill)) {
      if (fillRef->subtreeInfo &&
          fillRef->reference.handle.try_get<components::ComputedPatternComponent>()) {
        renderPattern(view, registry, instance, *fillRef, /*forStroke=*/false);
        subtreeConsumedBySubRendering = true;
      }
    }
    if (const auto* strokeRef =
            std::get_if<components::PaintResolvedReference>(&instance.resolvedStroke)) {
      if (strokeRef->subtreeInfo &&
          strokeRef->reference.handle.try_get<components::ComputedPatternComponent>()) {
        renderPattern(view, registry, instance, *strokeRef, /*forStroke=*/true);
        subtreeConsumedBySubRendering = true;
      }
    }

    const PaintParams paint = toPaintParams(registry, instance, style);
    renderer_.setPaint(paint);

    if (instance.visible && !filterHidesElement) {
      if (const auto* path =
              instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
        renderer_.drawPath(toPathShape(*path, style), paint.strokeParams);
        drawMarkers(view, registry, instance, *path, style);
      } else if (const auto* text =
                     instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
        const auto* textComp =
            instance.dataHandle(registry).try_get<components::TextComponent>();
        const TextParams textParams = toTextParams(registry, instance, style, textComp);
        renderer_.drawText(*text, textParams);
      } else if (const auto* svgImage =
                     instance.dataHandle(registry)
                         .try_get<components::LoadedSVGImageComponent>()) {
        // SVG sub-document referenced by <image>.
        if (svgImage->subDocument != nullptr) {
          const auto* sizedElement =
              instance.dataHandle(registry)
                  .try_get<components::ComputedSizedElementComponent>();
          if (sizedElement != nullptr) {
            const auto* preserveAspectRatioComp =
                instance.dataHandle(registry)
                    .try_get<components::PreserveAspectRatioComponent>();
            const PreserveAspectRatio aspectRatio =
                preserveAspectRatioComp != nullptr
                    ? preserveAspectRatioComp->preserveAspectRatio
                    : PreserveAspectRatio::Default();
            const double opacity = style.properties->opacity.getRequired();

            auto* subDoc = std::any_cast<SVGDocument>(svgImage->subDocument);
            if (subDoc != nullptr) {
              drawSubDocument(*subDoc, sizedElement->bounds, aspectRatio, opacity,
                              layerBaseTransform_ * instance.entityFromWorldTransform);
            }
          }
        }
      } else if (const auto* externalUse =
                     instance.dataHandle(registry)
                         .try_get<components::ExternalUseComponent>()) {
        // External SVG sub-document referenced by <use>.
        if (externalUse->subDocument != nullptr) {
          auto* subDoc = std::any_cast<SVGDocument>(externalUse->subDocument);
          if (subDoc != nullptr) {
            // Pass the <use> element's fill/stroke as context-fill/context-stroke
            // for the sub-document (SVG2 context paint inheritance).
            setSubDocumentContextPaint(*subDoc, instance.resolvedFill,
                                       instance.resolvedStroke);

            if (!externalUse->fragment.empty()) {
              // Fragment reference: render only the referenced element from the sub-document.
              // Pass the <use> element's absolute transform so the fragment is positioned
              // at the <use> element's location in the parent document.
              const Transformd parentAbsoluteTransform =
                  layerBaseTransform_ * instance.entityFromWorldTransform;
              drawSubDocumentElement(*subDoc, externalUse->fragment,
                                     parentAbsoluteTransform,
                                     style.properties->opacity.getRequired());
            } else {
              // Whole-document reference: render the entire sub-document.
              // For <use>, the entity's position is already captured via the renderer's
              // current transform (used by pushClip). Only pass the layer base transform
              // to include parent device scaling without the <use> element's own position.
              const Vector2i subDocSize = subDoc->canvasSize();
              const Boxd viewportBounds =
                  Boxd::WithSize(Vector2d(subDocSize.x, subDocSize.y));
              drawSubDocument(*subDoc, viewportBounds, PreserveAspectRatio::Default(),
                              style.properties->opacity.getRequired(),
                              layerBaseTransform_);
            }
          }
        }
      } else if (const auto* image =
                     instance.dataHandle(registry).try_get<components::LoadedImageComponent>()) {
        const std::optional<ImageParams> imageParams =
            toImageParams(instance, style, *image, registry);
        if (imageParams.has_value()) {
          const auto* sizedElement =
              instance.dataHandle(registry).try_get<components::ComputedSizedElementComponent>();
          const auto* preserveAspectRatio =
              instance.dataHandle(registry).try_get<components::PreserveAspectRatioComponent>();

          if (sizedElement != nullptr) {
            ResolvedClip imageClip;
            imageClip.clipRect = sizedElement->bounds;
            renderer_.pushClip(imageClip);

            const PreserveAspectRatio aspectRatio = preserveAspectRatio != nullptr
                                                        ? preserveAspectRatio->preserveAspectRatio
                                                        : PreserveAspectRatio::Default();
            const Transformd imageFromLocal = aspectRatio.elementContentFromViewBoxTransform(
                sizedElement->bounds, imageParams->targetRect);
            renderer_.pushTransform(imageFromLocal);
            renderer_.drawImage(*image->image, *imageParams);
            renderer_.popTransform();
            renderer_.popClip();
          } else {
            renderer_.drawImage(*image->image, *imageParams);
          }
        }
      }
    }

    // If this element starts a subtree (e.g., a group with isolation layers), defer cleanup
    // until the last child of the subtree is processed.
    // Elements with subtreeInfo always have layerDepth > 0 (viewport clip, opacity, clip-path,
    // etc.), so defer cleanup until the last child is processed. Leaf elements (no subtreeInfo)
    // have no clips or layers to pop. The entity transform uses setTransform (absolute, no
    // save/restore), so it never needs push/pop.
    //
    // However, if the subtree was already fully consumed by mask/pattern/marker rendering
    // (traverseRange/skipUntil advanced the view past lastRenderedEntity), the main loop will
    // never encounter lastRenderedEntity. In that case, pop immediately.
    const bool subtreeConsumed = instance.subtreeInfo && subtreeConsumedBySubRendering;
    const bool shouldDefer = instance.subtreeInfo && !subtreeConsumed;
    if (shouldDefer) {
      DeferredPop deferred;
      deferred.lastEntity = instance.subtreeInfo->lastRenderedEntity;
      deferred.hasViewportClip = hasViewportClip;
      deferred.hasIsolatedLayer = hasIsolatedLayer;
      deferred.hasFilterLayer = hasFilterLayer;
      deferred.hasEntityClip = hasEntityClip;
      deferred.maskDepth = maskDepth;
      subtreeMarkers_.push_back(deferred);
    } else {
      for (int mi = 0; mi < maskDepth; ++mi) {
        renderer_.popMask();
      }
      // Pop in reverse of push order: filter is innermost, then entity clip.
      if (hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (hasEntityClip) {
        renderer_.popClip();
      }
      if (hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
      if (hasViewportClip) {
        renderer_.popClip();
      }
    }

    // Pop deferred subtree layers when we reach their last entity.
    while (!subtreeMarkers_.empty() && subtreeMarkers_.back().lastEntity == entity) {
      const DeferredPop& deferred = subtreeMarkers_.back();
      for (int mi = 0; mi < deferred.maskDepth; ++mi) {
        renderer_.popMask();
      }
      // Pop in reverse of push order: filter is innermost, then entity clip.
      if (deferred.hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (deferred.hasEntityClip) {
        renderer_.popClip();
      }
      if (deferred.hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
      if (deferred.hasViewportClip) {
        renderer_.popClip();
      }
      subtreeMarkers_.pop_back();
    }
  }
}

void RendererDriver::traverseRange(RenderingInstanceView& view, Registry& registry,
                                   Entity startEntity, Entity endEntity) {
  // Advance to the start entity.
  while (!view.done() && view.currentEntity() != startEntity) {
    view.advance();
  }

  // Use a local deferred-pop stack for the marker subtree.
  std::vector<DeferredPop> localDeferred;

  // Draw until (and including) the end entity.
  bool foundEnd = false;
  while (!view.done() && !foundEnd) {
    foundEnd = view.currentEntity() == endEntity;

    const components::RenderingInstanceComponent& instance = view.get();
    const Entity entity = view.currentEntity();
    view.advance();

    const auto& style = instance.styleHandle(registry).get<components::ComputedStyleComponent>();
    if (!style.properties.has_value()) {
      continue;
    }

    // Apply the layer base transform composed with the entity's world transform.
    if (verbose_) {
      const Transformd combined = layerBaseTransform_ * instance.entityFromWorldTransform;
      std::cout << "[traverseRange] entity=" << entt::to_integral(entity)
                << " visible=" << instance.visible
                << "\n  layerBase=" << layerBaseTransform_
                << "\n  entityFromWorld=" << instance.entityFromWorldTransform
                << "\n  combined=" << combined << "\n";
    }
    renderer_.setTransform(layerBaseTransform_ * instance.entityFromWorldTransform);

    const double opacity = style.properties->opacity.getRequired();
    const MixBlendMode blendMode = style.properties->mixBlendMode.getRequired();
    const Isolation isolation = style.properties->isolation.getRequired();
    const bool hasIsolatedLayer =
        opacity < 1.0 || blendMode != MixBlendMode::Normal || isolation == Isolation::Isolate;
    if (hasIsolatedLayer) {
      renderer_.pushIsolatedLayer(opacity, blendMode);
    }

    std::optional<components::FilterGraph> filterGraph =
        instance.resolvedFilter.has_value()
            ? resolveFilterGraph(registry, instance.resolvedFilter.value(),
                                 style.properties->color.getRequired().rgba())
            : std::nullopt;
    const std::optional<Boxd> filterRegion =
        instance.resolvedFilter.has_value()
            ? computeFilterRegion(registry, instance.resolvedFilter.value(), instance)
            : std::nullopt;
    if (filterGraph.has_value()) {
      filterGraph->filterRegion = filterRegion;
      if (filterGraph->primitiveUnits == PrimitiveUnits::ObjectBoundingBox) {
        filterGraph->elementBoundingBox =
            components::ShapeSystem().getShapeBounds(instance.dataHandle(registry));
      }
      const Boxd viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
      if (viewBox.width() > 0 && viewBox.height() > 0) {
        filterGraph->userToPixelScale =
            Vector2d(renderingSize_.x / viewBox.width(), renderingSize_.y / viewBox.height());
      }
    }
    const bool hasFilterLayer = filterGraph.has_value() && !filterGraph->empty();
    const bool filterHidesElement =
        instance.resolvedFilter.has_value() && !hasFilterLayer;

    // Per SVG spec: paint → filter → clip-path. Push clip before filter so clip applies to
    // the filter output. The filter layer saves/clears the clip mask internally.
    ResolvedClip entityClip = toResolvedClip(instance, style, registry);
    entityClip.clipRect = std::nullopt;
    // Mask is handled separately below; don't let pushClip see it.
    // Mask is handled separately from clip — access it directly from instance.
    const bool hasEntityClip = !entityClip.empty();
    if (hasEntityClip) {
      renderer_.pushClip(entityClip);
    }

    if (hasFilterLayer) {
      preRenderSvgFeImages(*filterGraph);
      preRenderFeImageFragments(*filterGraph, registry);
      renderer_.pushFilterLayer(*filterGraph, filterRegion);
    }

    // Render mask content, then transition to masked content layer.
    int maskDepth = 0;
    if (instance.mask.has_value() && instance.mask->valid()) {
      maskDepth = renderMask(view, registry, instance, *instance.mask);
    }

    // Render pattern subtrees before drawing so the pattern shader is available.
    if (const auto* fillRef =
            std::get_if<components::PaintResolvedReference>(&instance.resolvedFill)) {
      if (fillRef->subtreeInfo &&
          fillRef->reference.handle.try_get<components::ComputedPatternComponent>()) {
        renderPattern(view, registry, instance, *fillRef, /*forStroke=*/false);
      }
    }
    if (const auto* strokeRef =
            std::get_if<components::PaintResolvedReference>(&instance.resolvedStroke)) {
      if (strokeRef->subtreeInfo &&
          strokeRef->reference.handle.try_get<components::ComputedPatternComponent>()) {
        renderPattern(view, registry, instance, *strokeRef, /*forStroke=*/true);
      }
    }

    const PaintParams paint = toPaintParams(registry, instance, style);
    renderer_.setPaint(paint);

    if (instance.visible && !filterHidesElement) {
      if (const auto* path =
              instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
        renderer_.drawPath(toPathShape(*path, style), paint.strokeParams);
      } else if (const auto* text =
                     instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
        const auto* textComp =
            instance.dataHandle(registry).try_get<components::TextComponent>();
        const TextParams textParams = toTextParams(registry, instance, style, textComp);
        renderer_.drawText(*text, textParams);
      }
    }

    if (instance.subtreeInfo) {
      DeferredPop deferred;
      deferred.lastEntity = instance.subtreeInfo->lastRenderedEntity;
      deferred.hasIsolatedLayer = hasIsolatedLayer;
      deferred.hasFilterLayer = hasFilterLayer;
      deferred.hasEntityClip = hasEntityClip;
      deferred.maskDepth = maskDepth;
      localDeferred.push_back(deferred);
    } else {
      // Pop in reverse of push order: mask innermost, then filter, clip, layer.
      for (int mi = 0; mi < maskDepth; ++mi) {
        renderer_.popMask();
      }
      if (hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (hasEntityClip) {
        renderer_.popClip();
      }
      if (hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
    }

    while (!localDeferred.empty() && localDeferred.back().lastEntity == entity) {
      const DeferredPop& deferred = localDeferred.back();
      // Pop in reverse of push order: mask innermost, then filter, clip, layer.
      for (int mi = 0; mi < deferred.maskDepth; ++mi) {
        renderer_.popMask();
      }
      if (deferred.hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (deferred.hasEntityClip) {
        renderer_.popClip();
      }
      if (deferred.hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
      localDeferred.pop_back();
    }
  }
}

void RendererDriver::skipUntil(RenderingInstanceView& view, Entity endEntity) {
  while (!view.done()) {
    const bool isEnd = view.currentEntity() == endEntity;
    view.advance();
    if (isEnd) {
      break;
    }
  }
}

int RendererDriver::renderMask(RenderingInstanceView& view, Registry& registry,
                               const components::RenderingInstanceComponent& instance,
                               const components::ResolvedMask& mask) {
  if (!mask.subtreeInfo) {
    return 0;
  }

  const EntityHandle maskHandle = mask.reference.handle;
  if (!maskHandle.valid()) {
    return 0;
  }

  const auto* maskComponent = maskHandle.try_get<components::MaskComponent>();
  if (maskComponent == nullptr) {
    return 0;
  }

  // Collect the mask chain outermost-first: [grandparent, parent, primary].
  // The chain is stored as primary->parentMask->parentMask->...
  SmallVector<const components::ResolvedMask*, 3> chain;
  for (const components::ResolvedMask* m = &mask; m != nullptr; m = m->parentMask.get()) {
    chain.push_back(m);
  }
  // Render in chain order (innermost first) to match the view's draw order.
  // The pushMask/popMask stack handles composition correctly: each popMask applies
  // its luminance to the content, and the LIFO stack naturally composes them.

  const Boxd shapeLocalBounds =
      components::ShapeSystem().getShapeBounds(instance.dataHandle(registry)).value_or(Boxd());

  // Render each mask in the chain, outermost first.
  for (const auto* m : chain) {
    if (!m->subtreeInfo || !m->reference.handle.valid()) {
      continue;
    }

    const auto* mc = m->reference.handle.try_get<components::MaskComponent>();
    if (!mc) {
      continue;
    }

    std::optional<Boxd> maskBounds;
    if (!mc->useAutoBounds()) {
      Boxd maskUnitsBounds;
      if (mc->maskUnits == MaskUnits::ObjectBoundingBox) {
        maskUnitsBounds = shapeLocalBounds;
      } else {
        maskUnitsBounds = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
      }

      const Lengthd x = mc->x.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
      const Lengthd y = mc->y.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
      const Lengthd width = mc->width.value_or(Lengthd(120.0, Lengthd::Unit::Percent));
      const Lengthd height = mc->height.value_or(Lengthd(120.0, Lengthd::Unit::Percent));

      const double xPx = x.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
      const double yPx = y.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);
      const double wPx = width.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
      const double hPx = height.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);

      maskBounds = Boxd::FromXYWH(xPx, yPx, wPx, hPx);
    }

    if (verbose_) {
      std::cout << "[renderMask] chain depth=" << chain.size()
                << " maskBounds=" << (maskBounds ? "yes" : "none")
                << "\n  layerBase=" << layerBaseTransform_
                << "\n  entityFromWorld=" << instance.entityFromWorldTransform
                << "\n  maskContentUnits="
                << (mc->maskContentUnits == MaskContentUnits::ObjectBoundingBox ? "OBB"
                                                                                : "userSpace")
                << "\n";
    }
    renderer_.pushMask(maskBounds);

    const Transformd savedLayerBase = layerBaseTransform_;
    layerBaseTransform_ = instance.entityFromWorldTransform;

    if (mc->maskContentUnits == MaskContentUnits::ObjectBoundingBox) {
      const Transformd userSpaceFromMaskContent =
          Transformd::Scale(shapeLocalBounds.size()) *
          Transformd::Translate(shapeLocalBounds.topLeft);
      layerBaseTransform_ = userSpaceFromMaskContent * layerBaseTransform_;
    }

    if (!shapeLocalBounds.isEmpty()) {
      traverseRange(view, registry, m->subtreeInfo->firstRenderedEntity,
                    m->subtreeInfo->lastRenderedEntity);
    } else {
      skipUntil(view, m->subtreeInfo->lastRenderedEntity);
    }

    layerBaseTransform_ = savedLayerBase;
    renderer_.transitionMaskToContent();
  }

  renderer_.setTransform(instance.entityFromWorldTransform);
  return static_cast<int>(chain.size());
}

void RendererDriver::renderPattern(RenderingInstanceView& view, Registry& registry,
                                   const components::RenderingInstanceComponent& instance,
                                   const components::PaintResolvedReference& ref,
                                   bool forStroke) {
  if (!ref.subtreeInfo) {
    return;
  }

  const EntityHandle target = ref.reference.handle;
  if (!target.valid()) {
    return;
  }

  const auto* computedPattern = target.try_get<components::ComputedPatternComponent>();
  if (computedPattern == nullptr) {
    return;
  }

  Boxd rect = computedPattern->tileRect;
  if (NearZero(rect.width()) || NearZero(rect.height())) {
    skipUntil(view, ref.subtreeInfo->lastRenderedEntity);
    return;
  }

  const Boxd viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
  const Boxd pathBounds =
      components::ShapeSystem().getShapeBounds(instance.dataHandle(registry)).value_or(Boxd());

  const bool objectBoundingBox =
      computedPattern->patternUnits == PatternUnits::ObjectBoundingBox;
  const bool patternContentObjectBoundingBox =
      computedPattern->patternContentUnits == PatternContentUnits::ObjectBoundingBox;

  if (objectBoundingBox) {
    if (NearZero(pathBounds.width()) || NearZero(pathBounds.height())) {
      skipUntil(view, ref.subtreeInfo->lastRenderedEntity);
      return;
    }

    const Vector2d rectSize = rect.size();
    rect.topLeft = rect.topLeft * pathBounds.size() + pathBounds.topLeft;
    rect.bottomRight = rectSize * pathBounds.size() + rect.topLeft;
  }

  Transformd patternContentFromPatternTile;
  if (computedPattern->viewBox) {
    patternContentFromPatternTile =
        computedPattern->preserveAspectRatio.elementContentFromViewBoxTransform(
            rect.toOrigin(), computedPattern->viewBox);
  } else if (patternContentObjectBoundingBox) {
    patternContentFromPatternTile = Transformd::Scale(pathBounds.size());
  }

  // Resolve the pattern's own transform (patternTransform attribute).
  const auto* maybeTransformComponent =
      target.try_get<components::ComputedLocalTransformComponent>();
  Transformd patternTransform;
  if (maybeTransformComponent) {
    const Vector2d origin = maybeTransformComponent->transformOrigin;
    patternTransform = Transformd::Translate(origin) *
                       maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics()) *
                       Transformd::Translate(-origin);
  }

  const Transformd patternTileFromTarget =
      Transformd::Translate(rect.topLeft) * patternTransform;

  renderer_.beginPatternTile(rect.toOrigin(), patternTileFromTarget);

  // Save and override layerBaseTransform for pattern content rendering.
  const Transformd savedLayerBase = layerBaseTransform_;
  layerBaseTransform_ = patternContentFromPatternTile;

  traverseRange(view, registry, ref.subtreeInfo->firstRenderedEntity,
                ref.subtreeInfo->lastRenderedEntity);

  layerBaseTransform_ = savedLayerBase;

  renderer_.endPatternTile(forStroke);
}

void RendererDriver::drawMarkers(RenderingInstanceView& view, Registry& registry,
                                 const components::RenderingInstanceComponent& instance,
                                 const components::ComputedPathComponent& path,
                                 const components::ComputedStyleComponent& style) {
  const bool hasMarkerStart = instance.markerStart.has_value();
  const bool hasMarkerMid = instance.markerMid.has_value();
  const bool hasMarkerEnd = instance.markerEnd.has_value();

  if (!hasMarkerStart && !hasMarkerMid && !hasMarkerEnd) {
    return;
  }

  const auto& commands = path.spline.commands();
  if (commands.size() < 2) {
    return;
  }

  const RenderingInstanceView::SavedState viewSnapshot = view.save();
  const std::vector<PathSpline::Vertex> vertices = path.spline.vertices();

  for (size_t i = 0; i < vertices.size(); ++i) {
    const PathSpline::Vertex& vertex = vertices[i];

    if (i == 0) {
      if (hasMarkerStart) {
        drawMarker(view, registry, instance, instance.markerStart.value(), vertex.point,
                   vertex.orientation, MarkerOrient::MarkerType::Start, style);
      }
    } else if (i == vertices.size() - 1) {
      if (hasMarkerEnd) {
        drawMarker(view, registry, instance, instance.markerEnd.value(), vertex.point,
                   vertex.orientation, MarkerOrient::MarkerType::Default, style);
      }
    } else if (hasMarkerMid) {
      drawMarker(view, registry, instance, instance.markerMid.value(), vertex.point,
                 vertex.orientation, MarkerOrient::MarkerType::Default, style);
    }

    view.restore(viewSnapshot);
  }

  // Skip past the marker definition entities to avoid double-rendering.
  if (hasMarkerEnd) {
    skipUntil(view, instance.markerEnd.value().subtreeInfo->lastRenderedEntity);
  } else if (hasMarkerMid) {
    skipUntil(view, instance.markerMid.value().subtreeInfo->lastRenderedEntity);
  } else if (hasMarkerStart) {
    skipUntil(view, instance.markerStart.value().subtreeInfo->lastRenderedEntity);
  }
}

void RendererDriver::drawMarker(RenderingInstanceView& view, Registry& registry,
                                const components::RenderingInstanceComponent& instance,
                                const components::ResolvedMarker& marker,
                                const Vector2d& vertexPosition, const Vector2d& direction,
                                MarkerOrient::MarkerType markerOrientType,
                                const components::ComputedStyleComponent& style) {
  const EntityHandle markerHandle = marker.reference.handle;
  if (!markerHandle.valid()) {
    return;
  }

  const auto* markerComponent = markerHandle.try_get<components::MarkerComponent>();
  if (markerComponent == nullptr) {
    return;
  }

  if (markerComponent->markerWidth <= 0.0 || markerComponent->markerHeight <= 0.0) {
    return;
  }

  const Boxd markerSize =
      Boxd::FromXYWH(0, 0, markerComponent->markerWidth, markerComponent->markerHeight);

  components::LayoutSystem layoutSystem;
  const std::optional<Boxd> markerViewBox =
      layoutSystem.overridesViewBox(markerHandle)
          ? std::optional<Boxd>(layoutSystem.getViewBox(markerHandle))
          : std::nullopt;
  const PreserveAspectRatio preserveAspectRatio =
      markerHandle.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;

  const double angleRadians =
      markerComponent->orient.computeAngleRadians(direction, markerOrientType);

  double markerScale = 1.0;
  if (markerComponent->markerUnits == MarkerUnits::StrokeWidth) {
    const double strokeWidth = style.properties->strokeWidth.getRequired().value;
    markerScale = strokeWidth;
  }

  const Transformd markerUnitsFromViewBox =
      preserveAspectRatio.elementContentFromViewBoxTransform(markerSize, markerViewBox);

  const Transformd markerOffsetFromVertex =
      Transformd::Translate(-markerComponent->refX * markerUnitsFromViewBox.data[0],
                            -markerComponent->refY * markerUnitsFromViewBox.data[3]);

  const Transformd vertexFromEntity = Transformd::Scale(markerScale) *
                                      Transformd::Rotate(angleRadians) *
                                      Transformd::Translate(vertexPosition);

  const Transformd vertexFromWorld =
      vertexFromEntity * layerBaseTransform_ * instance.entityFromWorldTransform;

  const Transformd markerUserSpaceFromWorld =
      Transformd::Scale(markerUnitsFromViewBox.data[0], markerUnitsFromViewBox.data[3]) *
      markerOffsetFromVertex * vertexFromWorld;

  // Save the current layer base transform and override it for the marker subtree.
  const Transformd savedLayerBase = layerBaseTransform_;
  layerBaseTransform_ = markerUserSpaceFromWorld;

  // Apply overflow clipping if needed. The clip rect is in world coordinates,
  // so reset the transform to identity before clipping.
  const auto& markerStyle = markerHandle.get<components::ComputedStyleComponent>();
  const Overflow overflow = markerStyle.properties->overflow.getRequired();
  const bool needsClip = overflow != Overflow::Visible && overflow != Overflow::Auto;

  if (needsClip) {
    renderer_.setTransform(Transformd());
    ResolvedClip markerClip;
    markerClip.clipRect =
        markerUserSpaceFromWorld.transformBox(markerViewBox.value_or(markerSize));
    renderer_.pushClip(markerClip);
  }

  if (marker.subtreeInfo) {
    traverseRange(view, registry, marker.subtreeInfo->firstRenderedEntity,
                  marker.subtreeInfo->lastRenderedEntity);
  }

  if (needsClip) {
    renderer_.popClip();
  }

  layerBaseTransform_ = savedLayerBase;
}

void RendererDriver::drawSubDocument(SVGDocument& subDocument, const Boxd& viewportBounds,
                                     const PreserveAspectRatio& aspectRatio, double opacity,
                                     const Transformd& parentAbsoluteTransform) {
  // Prepare the sub-document's render tree (styles, layout, resources).
  RendererUtils::prepareDocumentForRendering(subDocument, verbose_);

  // Determine the sub-document's intrinsic size for preserveAspectRatio mapping.
  const Vector2i subDocSize = subDocument.canvasSize();
  const Boxd subDocRect = Boxd::WithSize(Vector2d(subDocSize.x, subDocSize.y));

  // Clip to the <image> element's bounds.
  ResolvedClip imageClip;
  imageClip.clipRect = viewportBounds;
  renderer_.pushClip(imageClip);

  // Apply opacity as an isolated layer if needed.
  const bool needsOpacityLayer = opacity < 1.0;
  if (needsOpacityLayer) {
    renderer_.pushIsolatedLayer(opacity, MixBlendMode::Normal);
  }

  // Compute the base transform that maps sub-document coordinates into device space.
  // This composes:
  //   1. The parent element's absolute device transform (document-to-device scaling)
  //   2. preserveAspectRatio mapping from sub-doc viewport to the <image> bounds
  //   3. The sub-document's own viewBox-to-document transform
  // traverse() will compose this with each sub-document entity's entityFromWorldTransform.
  const Transformd subDocFromLocal =
      aspectRatio.elementContentFromViewBoxTransform(viewportBounds, subDocRect);
  const Transformd docFromCanvas = subDocument.documentFromCanvasTransform();
  // Compose the transform chain. Transformd operator* uses left-first order: A * B = "apply A,
  // then B". The sub-doc root entity's entityFromWorldTransform already includes docFromCanvas^-1
  // (mapping from document to canvas/device space). Including docFromCanvas here cancels that out,
  // so the net result maps sub-doc element coordinates through subDocFromLocal (to parent document
  // space), then through parentAbsoluteTransform (to device space).
  const Transformd baseTransform =
      subDocFromLocal * parentAbsoluteTransform * docFromCanvas;

  // Save and override layerBaseTransform for sub-document rendering.
  const Transformd savedLayerBase = layerBaseTransform_;
  layerBaseTransform_ = baseTransform;

  // Traverse the sub-document's render tree, emitting draw calls to the same renderer.
  RenderingInstanceView subView(subDocument.registry());
  traverse(subView, subDocument.registry());

  layerBaseTransform_ = savedLayerBase;

  if (needsOpacityLayer) {
    renderer_.popIsolatedLayer();
  }

  renderer_.popClip();
}

void RendererDriver::drawSubDocumentElement(SVGDocument& subDocument, std::string_view fragmentId,
                                            const Transformd& parentAbsoluteTransform,
                                            double opacity) {
  // Prepare the sub-document's render tree.
  RendererUtils::prepareDocumentForRendering(subDocument, verbose_);

  // Find the referenced element by ID.
  auto& docCtx = subDocument.registry().ctx().get<const components::SVGDocumentContext>();
  const Entity targetEntity = docCtx.getEntityById(RcString(fragmentId));
  if (targetEntity == entt::null) {
    if (verbose_) {
      std::cerr << "[drawSubDocumentElement] Fragment '" << fragmentId << "' not found\n";
    }
    return;
  }

  // Find the target's RenderingInstanceComponent in the sub-document's render tree.
  const auto* targetInstance =
      subDocument.registry().try_get<components::RenderingInstanceComponent>(targetEntity);
  if (targetInstance == nullptr) {
    if (verbose_) {
      std::cerr << "[drawSubDocumentElement] No RenderingInstanceComponent for fragment '"
                << fragmentId << "'\n";
    }
    return;
  }

  // Determine the subtree range: from the target entity to its last descendant.
  const Entity lastEntity = targetInstance->subtreeInfo
                                ? targetInstance->subtreeInfo->lastRenderedEntity
                                : targetEntity;

  // Apply opacity as an isolated layer if needed.
  const bool needsOpacityLayer = opacity < 1.0;
  if (needsOpacityLayer) {
    renderer_.pushIsolatedLayer(opacity, MixBlendMode::Normal);
  }

  // Set layerBaseTransform to position the target element at the <use> element's location.
  // The target's entityFromWorldTransform includes all ancestor transforms from the
  // sub-document root. By composing parentAbsoluteTransform with the inverse of the target's
  // transform, we strip the sub-document ancestors and place the element at the <use> position.
  // For each entity in the subtree:
  //   finalTransform = parentAbsolute * inverse(target) * entity = parentAbsolute * entityFromTarget
  const Transformd savedLayerBase = layerBaseTransform_;
  layerBaseTransform_ = parentAbsoluteTransform * targetInstance->entityFromWorldTransform.inverse();

  // Traverse only the target element's subtree from the sub-document's render tree.
  RenderingInstanceView subView(subDocument.registry());
  traverseRange(subView, subDocument.registry(), targetEntity, lastEntity);

  layerBaseTransform_ = savedLayerBase;

  if (needsOpacityLayer) {
    renderer_.popIsolatedLayer();
  }
}

void RendererDriver::setSubDocumentContextPaint(
    SVGDocument& subDocument, const components::ResolvedPaintServer& contextFill,
    const components::ResolvedPaintServer& contextStroke) {
  if (!subDocument.registry().ctx().contains<components::RenderingContext>()) {
    subDocument.registry().ctx().emplace<components::RenderingContext>(subDocument.registry());
  }
  auto& renderCtx = subDocument.registry().ctx().get<components::RenderingContext>();
  renderCtx.setInitialContextPaint(
      std::any(contextFill), std::any(contextStroke));
}

void RendererDriver::preRenderSvgFeImages(components::FilterGraph& filterGraph) {
  for (auto& node : filterGraph.nodes) {
    auto* imageNode = std::get_if<components::filter_primitive::Image>(&node.primitive);
    if (!imageNode || !imageNode->svgSubDocument || !imageNode->imageData.empty()) {
      continue;
    }

    // Cast the type-erased sub-document pointer.
    auto* subDoc = std::any_cast<SVGDocument>(imageNode->svgSubDocument);
    if (!subDoc) {
      continue;
    }

    // Create an independent offscreen renderer to render the sub-document.
    auto offscreen = renderer_.createOffscreenInstance();
    if (!offscreen) {
      if (verbose_) {
        std::cerr << "[preRenderSvgFeImages] Backend does not support offscreen rendering\n";
      }
      continue;
    }

    // Render the sub-document into the offscreen renderer.
    // draw() handles prepareDocumentForRendering, beginFrame, traverse, and endFrame internally.
    RendererDriver subDriver(*offscreen, verbose_);
    subDriver.draw(*subDoc);

    // Determine the rendered size from the sub-document's canvas size.
    const Vector2i subDocSize = subDoc->canvasSize();
    if (subDocSize.x <= 0 || subDocSize.y <= 0) {
      continue;
    }

    // Capture the rendered pixels.
    RendererBitmap snapshot = offscreen->takeSnapshot();
    if (!snapshot.empty()) {
      imageNode->imageWidth = snapshot.dimensions.x;
      imageNode->imageHeight = snapshot.dimensions.y;

      // The snapshot may have row padding. Convert to tightly packed RGBA.
      const size_t tightRowBytes = static_cast<size_t>(snapshot.dimensions.x) * 4;
      if (snapshot.rowBytes == tightRowBytes) {
        imageNode->imageData = std::move(snapshot.pixels);
      } else {
        imageNode->imageData.resize(tightRowBytes * snapshot.dimensions.y);
        for (int y = 0; y < snapshot.dimensions.y; ++y) {
          std::memcpy(imageNode->imageData.data() + y * tightRowBytes,
                      snapshot.pixels.data() + y * snapshot.rowBytes, tightRowBytes);
        }
      }
    }

    // Clear the sub-document pointer since we've now rendered to pixels.
    imageNode->svgSubDocument = nullptr;
  }
}

void RendererDriver::preRenderFeImageFragments(components::FilterGraph& filterGraph,
                                                Registry& registry) {
  // Lazily create the recursion guard set if needed.
  std::unordered_set<entt::id_type> localGuard;
  const bool ownsGuard = (feImageFragmentGuard_ == nullptr);
  if (ownsGuard) {
    feImageFragmentGuard_ = &localGuard;
  }

  for (auto& node : filterGraph.nodes) {
    auto* imageNode = std::get_if<components::filter_primitive::Image>(&node.primitive);
    if (!imageNode || imageNode->fragmentId.empty() || !imageNode->imageData.empty()) {
      continue;
    }

    // Look up the referenced element by ID.
    const auto& docCtx = registry.ctx().get<const components::SVGDocumentContext>();
    const Entity targetEntity = docCtx.getEntityById(imageNode->fragmentId);
    if (targetEntity == entt::null) {
      if (verbose_) {
        std::cerr << "[preRenderFeImageFragments] Fragment '" << imageNode->fragmentId
                  << "' not found\n";
      }
      imageNode->fragmentId = RcString();
      continue;
    }

    // Recursion guard: skip if this entity is already being rendered as an feImage fragment.
    const auto entityId = entt::to_integral(targetEntity);
    if (feImageFragmentGuard_->count(entityId) > 0) {
      if (verbose_) {
        std::cerr << "[preRenderFeImageFragments] Skipping recursive reference to '"
                  << imageNode->fragmentId << "'\n";
      }
      imageNode->fragmentId = RcString();
      continue;
    }

    // Create an offscreen renderer at the same canvas size.
    auto offscreen = renderer_.createOffscreenInstance();
    if (!offscreen) {
      if (verbose_) {
        std::cerr << "[preRenderFeImageFragments] Backend does not support offscreen rendering\n";
      }
      continue;
    }

    // Add to recursion guard before rendering.
    feImageFragmentGuard_->insert(entityId);

    // Check if the element is in the normal render tree.
    const auto* targetInstance =
        registry.try_get<components::RenderingInstanceComponent>(targetEntity);

    // For elements NOT in the render tree (e.g., inside <defs>), create a temporary
    // standalone render tree for the target subtree.
    const bool needsStandaloneRender = (targetInstance == nullptr);
    Entity standaloneLastEntity = entt::null;
    if (needsStandaloneRender) {
      if (!registry.ctx().contains<components::RenderingContext>()) {
        registry.ctx().emplace<components::RenderingContext>(registry);
      }
      auto& renderCtx = registry.ctx().get<components::RenderingContext>();
      standaloneLastEntity =
          renderCtx.instantiateSubtreeForStandaloneRender(targetEntity, verbose_);
      // Re-query after instantiation.
      targetInstance = registry.try_get<components::RenderingInstanceComponent>(targetEntity);
    }

    if (targetInstance == nullptr) {
      feImageFragmentGuard_->erase(entityId);
      imageNode->fragmentId = RcString();
      continue;
    }

    // Determine the last entity in the subtree for traverseRange.
    Entity lastEntity;
    if (needsStandaloneRender) {
      lastEntity = standaloneLastEntity;
    } else if (targetInstance->subtreeInfo) {
      lastEntity = targetInstance->subtreeInfo->lastRenderedEntity;
    } else {
      lastEntity = targetEntity;
    }

    // Begin frame with same viewport.
    RenderViewport viewport;
    viewport.size = Vector2d(renderingSize_.x, renderingSize_.y);
    viewport.devicePixelRatio = 1.0;
    offscreen->beginFrame(viewport);

    // Create a sub-driver for offscreen rendering and share the recursion guard.
    RendererDriver subDriver(*offscreen, verbose_);
    subDriver.renderingSize_ = renderingSize_;
    subDriver.feImageFragmentGuard_ = feImageFragmentGuard_;

    {
      RenderingInstanceView subView(registry);
      subDriver.traverseRange(subView, registry, targetEntity, lastEntity);
    }

    offscreen->endFrame();

    // Capture the rendered pixels.
    RendererBitmap snapshot = offscreen->takeSnapshot();
    if (!snapshot.empty()) {
      imageNode->imageWidth = snapshot.dimensions.x;
      imageNode->imageHeight = snapshot.dimensions.y;

      // Convert to tightly packed RGBA.
      const size_t tightRowBytes = static_cast<size_t>(snapshot.dimensions.x) * 4;
      if (snapshot.rowBytes == tightRowBytes) {
        imageNode->imageData = std::move(snapshot.pixels);
      } else {
        imageNode->imageData.resize(tightRowBytes * snapshot.dimensions.y);
        for (int y = 0; y < snapshot.dimensions.y; ++y) {
          std::memcpy(imageNode->imageData.data() + y * tightRowBytes,
                      snapshot.pixels.data() + y * snapshot.rowBytes, tightRowBytes);
        }
      }
    }

    // Remove from recursion guard after rendering.
    feImageFragmentGuard_->erase(entityId);

    // If we created standalone render instances, remove them to restore the original state.
    if (needsStandaloneRender) {
      // Collect all entities that have RenderingInstanceComponent with draw orders >= the
      // standalone instances. Since standalone instances were added after the main render tree,
      // they have the highest draw orders. We can find them by checking if they existed before.
      // Simplest approach: iterate all instances and remove any on non-renderable entities.
      std::vector<Entity> toRemove;
      for (auto entity : registry.view<components::RenderingInstanceComponent>()) {
        // If the entity's data handle has Nonrenderable behavior, it was added by standalone render.
        const auto* behavior =
            registry.try_get<components::RenderingBehaviorComponent>(entity);
        if (behavior && behavior->behavior == components::RenderingBehavior::Nonrenderable) {
          toRemove.push_back(entity);
        }
      }
      // Also check shadow entities — the target and its children may be shadow entities
      // whose light entities are Nonrenderable.
      if (registry.all_of<components::RenderingInstanceComponent>(targetEntity)) {
        toRemove.push_back(targetEntity);
      }
      for (Entity e : toRemove) {
        registry.remove<components::RenderingInstanceComponent>(e);
      }
    }

    // Clear fragmentId since we've rendered to pixels.
    imageNode->fragmentId = RcString();
  }

  // Clean up owned guard.
  if (ownsGuard) {
    feImageFragmentGuard_ = nullptr;
  }
}

}  // namespace donner::svg
