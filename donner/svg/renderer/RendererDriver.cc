#include "donner/svg/renderer/RendererDriver.h"

#include <iostream>
#include <optional>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/base/ParseError.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/RenderingContext.h"

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
  clip.mask = instance.mask;

  if (const auto* clipPaths =
          instance.styleHandle(registry).try_get<components::ComputedClipPathsComponent>()) {
    clip.clipPaths.reserve(clipPaths->clipPaths.size());
    for (const auto& path : clipPaths->clipPaths) {
      PathShape shape;
      shape.path = path.path;
      shape.fillRule = path.clipRule == ClipRule::NonZero ? FillRule::NonZero : FillRule::EvenOdd;
      clip.clipPaths.push_back(shape);
    }
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
                        const components::ComputedStyleComponent& style) {
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

  params.fontFamilies = properties.fontFamily.getRequiredRef();
  params.fontSize = properties.fontSize.getRequired();
  params.viewBox = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
  params.fontMetrics = FontMetrics();

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
  components::RenderingContext renderingContext(document.registry());
  RendererUtils::prepareDocumentForRendering(document, verbose_, verbose_ ? &warnings : nullptr);
  renderingContext.instantiateRenderTree(verbose_, verbose_ ? &warnings : nullptr);

  if (!warnings.empty()) {
    for (const ParseError& warning : warnings) {
      std::cerr << warning << '\n';
    }
  }

  const Vector2i renderingSize = document.canvasSize();
  RenderViewport viewport;
  viewport.size = Vector2d(renderingSize.x, renderingSize.y);
  viewport.devicePixelRatio = 1.0;

  renderer_.beginFrame(viewport);
  RenderingInstanceView view(document.registry());
  traverse(view, document.registry());
  renderer_.endFrame();
}

RendererBitmap RendererDriver::takeSnapshot() const {
  return renderer_.takeSnapshot();
}

void RendererDriver::traverse(RenderingInstanceView& view, Registry& registry) {
  while (!view.done()) {
    const components::RenderingInstanceComponent& instance = view.get();
    view.advance();

    const auto& style = instance.styleHandle(registry).get<components::ComputedStyleComponent>();
    if (!style.properties.has_value()) {
      continue;
    }

    renderer_.pushTransform(instance.entityFromWorldTransform);

    ResolvedClip clip = toResolvedClip(instance, style, registry);
    const bool hasClip = !clip.empty();
    if (hasClip) {
      renderer_.pushClip(clip);
    }

    const PaintParams paint = toPaintParams(registry, instance, style);
    renderer_.setPaint(paint);

    if (const auto* path =
            instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
      renderer_.drawPath(toPathShape(*path, style), paint.strokeParams);
    } else if (const auto* text =
                   instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
      const TextParams textParams = toTextParams(registry, instance, style);
      renderer_.drawText(*text, textParams);
    } else if (const auto* image =
                   instance.dataHandle(registry).try_get<components::LoadedImageComponent>()) {
      const std::optional<ImageParams> imageParams =
          toImageParams(instance, style, *image, registry);
      if (!imageParams.has_value()) {
        continue;
      }

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

    if (hasClip) {
      renderer_.popClip();
    }

    renderer_.popTransform();
  }
}

}  // namespace donner::svg
