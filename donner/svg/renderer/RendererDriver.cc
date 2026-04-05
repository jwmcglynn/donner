#include "donner/svg/renderer/RendererDriver.h"

#include <iostream>
#include <optional>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/base/ParseError.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/Overflow.h"
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
  clip.mask = instance.mask;

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

std::span<const FilterEffect> resolveFilterEffects(Registry& registry,
                                                   const components::ResolvedFilterEffect& filter) {
  if (const auto* effects = std::get_if<std::vector<FilterEffect>>(&filter)) {
    return *effects;
  }

  if (const auto* reference = std::get_if<ResolvedReference>(&filter)) {
    if (const auto* computed = registry.try_get<components::ComputedFilterComponent>(*reference)) {
      return computed->effectChain;
    }
  }

  return {};
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

  params.fontFamilies = properties.fontFamily.getRequired();
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
  RendererUtils::prepareDocumentForRendering(document, verbose_, verbose_ ? &warnings : nullptr);

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
    if (verbose_) {
      const Transformd combined = layerBaseTransform_ * instance.entityFromWorldTransform;
      std::cout << "[traverse] entity=" << entt::to_integral(entity)
                << " visible=" << instance.visible << " hasMask=" << instance.mask.has_value()
                << " hasSubtree=" << instance.subtreeInfo.has_value() << " transform=" << combined
                << "\n";
    }
    renderer_.setTransform(layerBaseTransform_ * instance.entityFromWorldTransform);

    const double opacity = style.properties->opacity.getRequired();
    const bool hasIsolatedLayer = opacity < 1.0;
    if (hasIsolatedLayer) {
      renderer_.pushIsolatedLayer(opacity);
    }

    const bool hasFilterLayer = instance.resolvedFilter.has_value();
    if (hasFilterLayer) {
      const std::span<const FilterEffect> effects =
          resolveFilterEffects(registry, instance.resolvedFilter.value());
      renderer_.pushFilterLayer(effects);
    }

    // Clip paths are in entity-local coordinates.
    ResolvedClip entityClip = toResolvedClip(instance, style, registry);
    entityClip.clipRect = std::nullopt;  // Already handled above as viewport clip.
    // Mask is handled separately below; don't let pushClip see it.
    const std::optional<components::ResolvedMask> entityMask = entityClip.mask;
    entityClip.mask = std::nullopt;
    const bool hasEntityClip = !entityClip.empty();
    if (hasEntityClip) {
      renderer_.pushClip(entityClip);
    }

    // Render mask content, then transition to masked content layer.
    const bool hasMask = entityMask.has_value() && entityMask->valid();
    // Track whether mask/pattern rendering consumed the element's subtree entities.
    bool subtreeConsumedBySubRendering = false;

    if (hasMask) {
      renderMask(view, registry, instance, *entityMask);
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

    if (instance.visible) {
      if (const auto* path =
              instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
        renderer_.drawPath(toPathShape(*path, style), paint.strokeParams);
        drawMarkers(view, registry, instance, *path, style);
      } else if (const auto* text =
                     instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
        const TextParams textParams = toTextParams(registry, instance, style);
        renderer_.drawText(*text, textParams);
      } else if (const auto* svgImage =
                     instance.dataHandle(registry).try_get<components::LoadedSVGImageComponent>()) {
        // SVG sub-document referenced by <image>.
        const auto* sizedElement =
            instance.dataHandle(registry).try_get<components::ComputedSizedElementComponent>();
        if (sizedElement != nullptr) {
          const auto* preserveAspectRatioComp =
              instance.dataHandle(registry).try_get<components::PreserveAspectRatioComponent>();
          const PreserveAspectRatio aspectRatio = preserveAspectRatioComp != nullptr
                                                      ? preserveAspectRatioComp->preserveAspectRatio
                                                      : PreserveAspectRatio::Default();

          SVGDocument subDocument = SVGDocument::CreateFromHandle(svgImage->subDocument);
          drawSubDocument(subDocument, sizedElement->bounds, aspectRatio,
                          style.properties->opacity.getRequired(),
                          layerBaseTransform_ * instance.entityFromWorldTransform);
        }
      } else if (const auto* externalUse =
                     instance.dataHandle(registry).try_get<components::ExternalUseComponent>()) {
        // External SVG sub-document referenced by <use>.
        // Pass the <use> element's fill/stroke as context-fill/context-stroke
        // for the sub-document (SVG2 context paint inheritance).
        SVGDocument subDocument = SVGDocument::CreateFromHandle(externalUse->subDocument);
        setSubDocumentContextPaint(subDocument, instance.resolvedFill,
                                   instance.resolvedStroke);

        if (!externalUse->fragment.empty()) {
          // Fragment reference: render only the referenced element from the sub-document.
          // Pass the <use> element's absolute transform so the fragment is positioned
          // at the <use> element's location in the parent document.
          const Transformd parentAbsoluteTransform =
              layerBaseTransform_ * instance.entityFromWorldTransform;
          drawSubDocumentElement(subDocument, externalUse->fragment,
                                 parentAbsoluteTransform, style.properties->opacity.getRequired());
        } else {
          // Whole-document reference: render the entire sub-document.
          // For <use>, the entity's position is already captured via the renderer's
          // current transform (used by pushClip). Only pass the layer base transform
          // to include parent device scaling without the <use> element's own position.
          const Vector2i subDocSize = subDocument.canvasSize();
          const Boxd viewportBounds = Boxd::WithSize(Vector2d(subDocSize.x, subDocSize.y));
          drawSubDocument(subDocument, viewportBounds, PreserveAspectRatio::Default(),
                          style.properties->opacity.getRequired(), layerBaseTransform_);
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
      deferred.hasMask = hasMask;
      subtreeMarkers_.push_back(deferred);
    } else {
      if (hasMask) {
        renderer_.popMask();
      }
      if (hasEntityClip) {
        renderer_.popClip();
      }
      if (hasFilterLayer) {
        renderer_.popFilterLayer();
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
      if (deferred.hasMask) {
        renderer_.popMask();
      }
      if (deferred.hasEntityClip) {
        renderer_.popClip();
      }
      if (deferred.hasFilterLayer) {
        renderer_.popFilterLayer();
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
                << " visible=" << instance.visible << "\n  layerBase=" << layerBaseTransform_
                << "\n  entityFromWorld=" << instance.entityFromWorldTransform
                << "\n  combined=" << combined << "\n";
    }
    renderer_.setTransform(layerBaseTransform_ * instance.entityFromWorldTransform);

    const double opacity = style.properties->opacity.getRequired();
    const bool hasIsolatedLayer = opacity < 1.0;
    if (hasIsolatedLayer) {
      renderer_.pushIsolatedLayer(opacity);
    }

    const bool hasFilterLayer = instance.resolvedFilter.has_value();
    if (hasFilterLayer) {
      const std::span<const FilterEffect> effects =
          resolveFilterEffects(registry, instance.resolvedFilter.value());
      renderer_.pushFilterLayer(effects);
    }

    ResolvedClip entityClip = toResolvedClip(instance, style, registry);
    entityClip.clipRect = std::nullopt;
    const bool hasEntityClip = !entityClip.empty();
    if (hasEntityClip) {
      renderer_.pushClip(entityClip);
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

    if (instance.visible) {
      if (const auto* path =
              instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
        renderer_.drawPath(toPathShape(*path, style), paint.strokeParams);
      } else if (const auto* text =
                     instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
        const TextParams textParams = toTextParams(registry, instance, style);
        renderer_.drawText(*text, textParams);
      } else if (const auto* svgImage =
                     instance.dataHandle(registry).try_get<components::LoadedSVGImageComponent>()) {
        // SVG sub-document referenced by <image>.
        const auto* sizedElement =
            instance.dataHandle(registry).try_get<components::ComputedSizedElementComponent>();
        if (sizedElement != nullptr) {
          const auto* preserveAspectRatioComp =
              instance.dataHandle(registry).try_get<components::PreserveAspectRatioComponent>();
          const PreserveAspectRatio aspectRatio = preserveAspectRatioComp != nullptr
                                                      ? preserveAspectRatioComp->preserveAspectRatio
                                                      : PreserveAspectRatio::Default();

          SVGDocument subDocument = SVGDocument::CreateFromHandle(svgImage->subDocument);
          drawSubDocument(subDocument, sizedElement->bounds, aspectRatio,
                          style.properties->opacity.getRequired(),
                          layerBaseTransform_ * instance.entityFromWorldTransform);
        }
      } else if (const auto* image =
                     instance.dataHandle(registry).try_get<components::LoadedImageComponent>()) {
        const std::optional<ImageParams> imageParams =
            toImageParams(instance, style, *image, registry);
        if (imageParams.has_value()) {
          renderer_.drawImage(*image->image, *imageParams);
        }
      }
    }

    if (instance.subtreeInfo) {
      DeferredPop deferred;
      deferred.lastEntity = instance.subtreeInfo->lastRenderedEntity;
      deferred.hasIsolatedLayer = hasIsolatedLayer;
      deferred.hasFilterLayer = hasFilterLayer;
      deferred.hasEntityClip = hasEntityClip;
      localDeferred.push_back(deferred);
    } else {
      if (hasEntityClip) {
        renderer_.popClip();
      }
      if (hasFilterLayer) {
        renderer_.popFilterLayer();
      }
      if (hasIsolatedLayer) {
        renderer_.popIsolatedLayer();
      }
    }

    while (!localDeferred.empty() && localDeferred.back().lastEntity == entity) {
      const DeferredPop& deferred = localDeferred.back();
      if (deferred.hasEntityClip) {
        renderer_.popClip();
      }
      if (deferred.hasFilterLayer) {
        renderer_.popFilterLayer();
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

void RendererDriver::renderMask(RenderingInstanceView& view, Registry& registry,
                                const components::RenderingInstanceComponent& instance,
                                const components::ResolvedMask& mask) {
  if (!mask.subtreeInfo) {
    return;
  }

  const EntityHandle maskHandle = mask.reference.handle;
  if (!maskHandle.valid()) {
    return;
  }

  const auto* maskComponent = maskHandle.try_get<components::MaskComponent>();
  if (maskComponent == nullptr) {
    return;
  }

  // Compute mask bounds.
  const Boxd shapeLocalBounds =
      components::ShapeSystem().getShapeBounds(instance.dataHandle(registry)).value_or(Boxd());

  std::optional<Boxd> maskBounds;
  if (!maskComponent->useAutoBounds()) {
    Boxd maskUnitsBounds;
    if (maskComponent->maskUnits == MaskUnits::ObjectBoundingBox) {
      maskUnitsBounds = shapeLocalBounds;
    } else {
      maskUnitsBounds = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
    }

    const Lengthd x = maskComponent->x.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
    const Lengthd y = maskComponent->y.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
    const Lengthd width = maskComponent->width.value_or(Lengthd(120.0, Lengthd::Unit::Percent));
    const Lengthd height = maskComponent->height.value_or(Lengthd(120.0, Lengthd::Unit::Percent));

    const double xPx = x.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
    const double yPx = y.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);
    const double wPx = width.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
    const double hPx = height.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);

    maskBounds = Boxd::FromXYWH(xPx, yPx, wPx, hPx);
  }

  if (verbose_) {
    std::cout << "[renderMask] maskBounds=" << (maskBounds ? "yes" : "none")
              << "\n  layerBase=" << layerBaseTransform_
              << "\n  entityFromWorld=" << instance.entityFromWorldTransform
              << "\n  maskContentUnits="
              << (maskComponent->maskContentUnits == MaskContentUnits::ObjectBoundingBox
                      ? "OBB"
                      : "userSpace")
              << "\n";
  }
  renderer_.pushMask(maskBounds);

  // Save and override layerBaseTransform for mask content rendering.
  const Transformd savedLayerBase = layerBaseTransform_;
  layerBaseTransform_ = instance.entityFromWorldTransform;

  // Apply maskContentUnits transform.
  if (maskComponent->maskContentUnits == MaskContentUnits::ObjectBoundingBox) {
    const Transformd userSpaceFromMaskContent = Transformd::Scale(shapeLocalBounds.size()) *
                                                Transformd::Translate(shapeLocalBounds.topLeft);
    layerBaseTransform_ = userSpaceFromMaskContent * layerBaseTransform_;
  }

  // Render mask subtree.
  if (!shapeLocalBounds.isEmpty()) {
    traverseRange(view, registry, mask.subtreeInfo->firstRenderedEntity,
                  mask.subtreeInfo->lastRenderedEntity);
  } else {
    skipUntil(view, mask.subtreeInfo->lastRenderedEntity);
  }

  layerBaseTransform_ = savedLayerBase;

  renderer_.transitionMaskToContent();

  // Restore the entity transform after mask rendering.
  renderer_.setTransform(instance.entityFromWorldTransform);
}

void RendererDriver::renderPattern(RenderingInstanceView& view, Registry& registry,
                                   const components::RenderingInstanceComponent& instance,
                                   const components::PaintResolvedReference& ref, bool forStroke) {
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

  const bool objectBoundingBox = computedPattern->patternUnits == PatternUnits::ObjectBoundingBox;
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

  const Transformd patternTileFromTarget = Transformd::Translate(rect.topLeft) * patternTransform;

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
    markerClip.clipRect = markerUserSpaceFromWorld.transformBox(markerViewBox.value_or(markerSize));
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
    renderer_.pushIsolatedLayer(opacity);
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
  // Transformd operator* is left-first: A * B means "apply A, then B".
  const Transformd baseTransform = subDocFromLocal * parentAbsoluteTransform * docFromCanvas;

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
    return;
  }

  // Find the target's RenderingInstanceComponent in the sub-document's render tree.
  const auto* targetInstance =
      subDocument.registry().try_get<components::RenderingInstanceComponent>(targetEntity);
  if (targetInstance == nullptr) {
    return;
  }

  // Determine the subtree range: from the target entity to its last descendant.
  const Entity lastEntity =
      targetInstance->subtreeInfo ? targetInstance->subtreeInfo->lastRenderedEntity : targetEntity;

  // Apply opacity as an isolated layer if needed.
  const bool needsOpacityLayer = opacity < 1.0;
  if (needsOpacityLayer) {
    renderer_.pushIsolatedLayer(opacity);
  }

  // Strip the sub-document ancestors and position the target subtree at the <use> location.
  const Transformd savedLayerBase = layerBaseTransform_;
  layerBaseTransform_ =
      parentAbsoluteTransform * targetInstance->entityFromWorldTransform.inverse();

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
  renderCtx.setInitialContextPaint(contextFill, contextStroke);
}

}  // namespace donner::svg
