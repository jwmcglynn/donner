#include "donner/svg/renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInternal.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

namespace {

void CollectSubtreeEntities(const SVGElement& element, std::unordered_set<Entity>& out) {
  out.insert(element.entityHandle().entity());
  for (std::optional<SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    CollectSubtreeEntities(*child, out);
  }
}

struct SubtreeRenderSpan {
  Entity firstEntity = entt::null;
  Entity lastEntity = entt::null;
  std::optional<Box2d> worldBounds;
};

struct DirtyFlagsSnapshot {
  Entity entity = entt::null;
  uint16_t flags = components::DirtyFlagsComponent::None;
};

struct RenderInvalidationSnapshot {
  bool hadRenderTreeState = false;
  components::RenderTreeState renderTreeState;
  std::vector<DirtyFlagsSnapshot> dirtyFlags;
};

std::optional<Box2d> IntersectBoxes(const Box2d& lhs, const Box2d& rhs) {
  const double left = std::max(lhs.topLeft.x, rhs.topLeft.x);
  const double top = std::max(lhs.topLeft.y, rhs.topLeft.y);
  const double right = std::min(lhs.bottomRight.x, rhs.bottomRight.x);
  const double bottom = std::min(lhs.bottomRight.y, rhs.bottomRight.y);
  if (right <= left || bottom <= top) {
    return std::nullopt;
  }
  return Box2d(Vector2d(left, top), Vector2d(right, bottom));
}

std::optional<Box2d> RootViewBoxCanvasBounds(SVGDocument& document, Registry& registry) {
  const std::optional<Box2d> viewBox = document.svgElement().viewBox();
  if (!viewBox.has_value()) {
    return std::nullopt;
  }

  const Transform2d canvasFromDocumentWorldTransform =
      components::LayoutSystem().getCanvasFromDocumentTransform(registry);
  return canvasFromDocumentWorldTransform.transformBox(*viewBox);
}

struct ThumbnailTransform {
  RenderViewport viewport;
  Transform2d surfaceFromCanvas;
};

Vector2i ComputeThumbnailBitmapSize(const Box2d& cropBounds, Vector2i maxSizePx) {
  const double availableW = std::max(1.0, static_cast<double>(maxSizePx.x));
  const double availableH = std::max(1.0, static_cast<double>(maxSizePx.y));
  const double boundsW = cropBounds.width();
  const double boundsH = cropBounds.height();
  const double scale = std::min(availableW / boundsW, availableH / boundsH);

  const int width = std::clamp(static_cast<int>(std::ceil(boundsW * scale)), 1, maxSizePx.x);
  const int height = std::clamp(static_cast<int>(std::ceil(boundsH * scale)), 1, maxSizePx.y);
  return Vector2i(width, height);
}

ThumbnailTransform ComputeThumbnailTransform(const Box2d& cropBounds, Vector2i sizePx,
                                             bool centerInViewport = true) {
  ThumbnailTransform result;
  result.viewport.size = Vector2d(sizePx.x, sizePx.y);
  result.viewport.devicePixelRatio = 1.0;

  const double availableW = std::max(1.0, result.viewport.size.x);
  const double availableH = std::max(1.0, result.viewport.size.y);

  const double boundsW = cropBounds.width();
  const double boundsH = cropBounds.height();
  const double scale = std::min(availableW / boundsW, availableH / boundsH);

  const double scaledW = boundsW * scale;
  const double scaledH = boundsH * scale;
  const Vector2d offset = centerInViewport ? Vector2d((result.viewport.size.x - scaledW) * 0.5,
                                                      (result.viewport.size.y - scaledH) * 0.5)
                                           : Vector2d::Zero();

  result.surfaceFromCanvas = Transform2d::Translate(-cropBounds.topLeft) *
                             Transform2d::Scale(scale) * Transform2d::Translate(offset);
  return result;
}

RendererBitmap RenderThumbnailRange(RendererInterface& renderer, Registry& registry,
                                    const SubtreeRenderSpan& span, const Box2d& cropBounds,
                                    Vector2i sizePx) {
  const ThumbnailTransform thumbnailTransform = ComputeThumbnailTransform(cropBounds, sizePx);
  RendererDriver driver(renderer);
  driver.drawEntityRange(registry, span.firstEntity, span.lastEntity, thumbnailTransform.viewport,
                         thumbnailTransform.surfaceFromCanvas);
  return driver.takeSnapshot();
}

bool SolidPaintContributesBounds(const PaintServer::Solid& solid, css::RGBA currentColor,
                                 double opacity) {
  if (opacity <= 0.0) {
    return false;
  }

  return solid.color.resolve(currentColor, static_cast<float>(opacity)).a > 0u;
}

bool ResolvedPaintContributesBounds(const components::ResolvedPaintServer& paint,
                                    css::RGBA currentColor, double opacity) {
  if (opacity <= 0.0 || std::holds_alternative<PaintServer::None>(paint)) {
    return false;
  }

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
    return SolidPaintContributesBounds(*solid, currentColor, opacity);
  }

  const auto* reference = std::get_if<components::PaintResolvedReference>(&paint);
  if (reference == nullptr) {
    return true;
  }

  if (reference->reference.valid()) {
    return true;
  }

  return reference->fallback.has_value() &&
         reference->fallback->resolve(currentColor, static_cast<float>(opacity)).a > 0u;
}

bool RenderingInstanceContributesBounds(const components::RenderingInstanceComponent& instance,
                                        const components::ComputedStyleComponent& style) {
  if (!style.properties.has_value()) {
    return true;
  }

  const auto& props = *style.properties;
  const double opacity = props.opacity.get().value();
  if (opacity <= 0.0) {
    return false;
  }

  const css::RGBA currentColor =
      props.color.get().value().resolve(css::RGBA::RGB(0, 0, 0), /*opacity=*/1.0f);
  const bool fillContributes = ResolvedPaintContributesBounds(
      instance.resolvedFill, currentColor, opacity * props.fillOpacity.get().value());
  const bool strokeContributes =
      props.strokeWidth.get().value().value > 0.0 &&
      ResolvedPaintContributesBounds(instance.resolvedStroke, currentColor,
                                     opacity * props.strokeOpacity.get().value());
  return fillContributes || strokeContributes;
}

bool PathCanHaveStrokeJoin(const Path& path) {
  int segmentCountInSubpath = 0;
  for (const Path::Command& command : path.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo: segmentCountInSubpath = 0; break;
      case Path::Verb::LineTo:
      case Path::Verb::QuadTo:
      case Path::Verb::CurveTo:
        if (segmentCountInSubpath >= 1) {
          return true;
        }
        ++segmentCountInSubpath;
        break;
      case Path::Verb::ClosePath:
        if (segmentCountInSubpath >= 2) {
          return true;
        }
        segmentCountInSubpath = 0;
        break;
    }
  }
  return false;
}

RenderInvalidationSnapshot CaptureRenderInvalidation(Registry& registry) {
  RenderInvalidationSnapshot snapshot;
  if (const auto* state = registry.ctx().find<components::RenderTreeState>()) {
    snapshot.hadRenderTreeState = true;
    snapshot.renderTreeState = *state;
  }

  for (const Entity entity : registry.view<components::DirtyFlagsComponent>()) {
    const auto& dirty = registry.get<components::DirtyFlagsComponent>(entity);
    snapshot.dirtyFlags.push_back(DirtyFlagsSnapshot{
        .entity = entity,
        .flags = dirty.flags,
    });
  }
  return snapshot;
}

void RestoreRenderInvalidation(Registry& registry, const RenderInvalidationSnapshot& snapshot) {
  registry.clear<components::DirtyFlagsComponent>();
  for (const DirtyFlagsSnapshot& dirty : snapshot.dirtyFlags) {
    if (!registry.valid(dirty.entity)) {
      continue;
    }
    registry.emplace_or_replace<components::DirtyFlagsComponent>(dirty.entity).flags = dirty.flags;
  }

  if (snapshot.hadRenderTreeState) {
    if (registry.ctx().contains<components::RenderTreeState>()) {
      registry.ctx().erase<components::RenderTreeState>();
    }
    registry.ctx().emplace<components::RenderTreeState>(snapshot.renderTreeState);
  } else if (registry.ctx().contains<components::RenderTreeState>()) {
    registry.ctx().erase<components::RenderTreeState>();
  }
}

class ScopedRenderInvalidationRestore {
public:
  explicit ScopedRenderInvalidationRestore(Registry& registry)
      : registry_(registry), snapshot_(CaptureRenderInvalidation(registry)) {}

  ~ScopedRenderInvalidationRestore() { RestoreRenderInvalidation(registry_, snapshot_); }

private:
  Registry& registry_;
  RenderInvalidationSnapshot snapshot_;
};

SubtreeRenderSpan ResolveSubtreeRenderSpan(Registry& registry,
                                           const std::unordered_set<Entity>& subtreeEntities) {
  SubtreeRenderSpan span;
  int firstDrawOrder = 0;
  int lastDrawOrder = 0;
  for (auto view = registry.view<const components::RenderingInstanceComponent>();
       auto storageEntity : view) {
    const auto& instance = view.get<const components::RenderingInstanceComponent>(storageEntity);
    if (subtreeEntities.count(instance.dataEntity) == 0) {
      continue;
    }

    if (span.firstEntity == entt::null || instance.drawOrder < firstDrawOrder) {
      firstDrawOrder = instance.drawOrder;
      span.firstEntity = storageEntity;
    }
    if (span.lastEntity == entt::null || instance.drawOrder > lastDrawOrder) {
      lastDrawOrder = instance.drawOrder;
      span.lastEntity = storageEntity;
    }

    if (!instance.visible) {
      continue;
    }
    const auto* path =
        instance.dataHandle(registry).template try_get<components::ComputedPathComponent>();
    if (path == nullptr) {
      continue;
    }
    const auto& style = instance.styleHandle(registry).get<components::ComputedStyleComponent>();
    if (!RenderingInstanceContributesBounds(instance, style)) {
      continue;
    }

    Box2d local = path->spline.bounds();
    if (style.properties.has_value()) {
      const auto& props = *style.properties;
      const double strokeWidth = props.strokeWidth.get().value().value;
      if (ResolvedPaintContributesBounds(
              instance.resolvedStroke,
              props.color.get().value().resolve(css::RGBA::RGB(0, 0, 0), /*opacity=*/1.0f),
              props.opacity.get().value() * props.strokeOpacity.get().value()) &&
          strokeWidth > 0.0) {
        double padding = strokeWidth * 0.5;
        if (props.strokeLinejoin.get().value() == StrokeLinejoin::Miter &&
            PathCanHaveStrokeJoin(path->spline)) {
          padding *= props.strokeMiterlimit.get().value();
        }
        local = Box2d(local.topLeft - Vector2d(padding, padding),
                      local.bottomRight + Vector2d(padding, padding));
      }
    }
    if (local.isEmpty()) {
      continue;
    }
    const Box2d world = instance.worldFromEntityTransform.transformBox(local);
    if (!span.worldBounds) {
      span.worldBounds = world;
    } else {
      span.worldBounds->addBox(world);
    }
  }
  return span;
}

}  // namespace

Renderer::Renderer(bool verbose) : impl_(CreateRendererImplementation(verbose)) {}

Renderer::Renderer(std::shared_ptr<geode::GeodeDevice> device, bool verbose)
    : impl_(CreateRendererImplementation(std::move(device), verbose)) {}

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer&&) noexcept = default;

Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::draw(SVGDocument& document) {
  impl_->draw(document);
}

RendererBitmap Renderer::renderElementToBitmap(SVGElement element, Vector2i sizePx) {
  if (sizePx.x <= 0 || sizePx.y <= 0) {
    return RendererBitmap{};
  }

  SVGDocument document = element.ownerDocument();
  DocumentWriteAccess access = document.writeAccess();
  Registry& registry = document.registry();
  ScopedRenderInvalidationRestore restoreInvalidation(registry);

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warnings);

  std::unordered_set<Entity> subtreeEntities;
  CollectSubtreeEntities(element, subtreeEntities);
  const SubtreeRenderSpan span = ResolveSubtreeRenderSpan(registry, subtreeEntities);
  if (span.firstEntity == entt::null || span.lastEntity == entt::null ||
      !span.worldBounds.has_value() || span.worldBounds->isEmpty()) {
    return RendererBitmap{};
  }

  Box2d cropBounds = *span.worldBounds;
  if (const std::optional<Box2d> rootCanvasBounds = RootViewBoxCanvasBounds(document, registry)) {
    const std::optional<Box2d> visibleBounds = IntersectBoxes(cropBounds, *rootCanvasBounds);
    if (!visibleBounds.has_value()) {
      return RendererBitmap{};
    }
    cropBounds = *visibleBounds;
  }

  std::unique_ptr<RendererInterface> offscreenRenderer = impl_->createOffscreenInstance();
  RendererInterface& thumbnailRenderer =
      offscreenRenderer != nullptr ? *offscreenRenderer : static_cast<RendererInterface&>(*impl_);

  const Vector2i bitmapSizePx = ComputeThumbnailBitmapSize(cropBounds, sizePx);
  return RenderThumbnailRange(thumbnailRenderer, registry, span, cropBounds, bitmapSizePx);
}

void Renderer::beginFrame(const RenderViewport& viewport) {
  impl_->beginFrame(viewport);
}

void Renderer::endFrame() {
  impl_->endFrame();
}

void Renderer::setTransform(const Transform2d& transform) {
  impl_->setTransform(transform);
}

void Renderer::pushTransform(const Transform2d& transform) {
  impl_->pushTransform(transform);
}

void Renderer::popTransform() {
  impl_->popTransform();
}

void Renderer::pushClip(const ResolvedClip& clip) {
  impl_->pushClip(clip);
}

void Renderer::popClip() {
  impl_->popClip();
}

void Renderer::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  impl_->pushIsolatedLayer(opacity, blendMode);
}

void Renderer::popIsolatedLayer() {
  impl_->popIsolatedLayer();
}

void Renderer::pushFilterLayer(const components::FilterGraph& filterGraph,
                               const std::optional<Box2d>& filterRegion) {
  impl_->pushFilterLayer(filterGraph, filterRegion);
}

void Renderer::popFilterLayer() {
  impl_->popFilterLayer();
}

void Renderer::pushMask(const std::optional<Box2d>& maskBounds) {
  impl_->pushMask(maskBounds);
}

void Renderer::transitionMaskToContent() {
  impl_->transitionMaskToContent();
}

void Renderer::popMask() {
  impl_->popMask();
}

void Renderer::beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) {
  impl_->beginPatternTile(tileRect, targetFromPattern);
}

void Renderer::endPatternTile(bool forStroke) {
  impl_->endPatternTile(forStroke);
}

void Renderer::setPaint(const PaintParams& paint) {
  impl_->setPaint(paint);
}

void Renderer::drawPath(const PathShape& path, const StrokeParams& stroke) {
  impl_->drawPath(path, stroke);
}

void Renderer::drawRect(const Box2d& rect, const StrokeParams& stroke) {
  impl_->drawRect(rect, stroke);
}

void Renderer::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
  impl_->drawEllipse(bounds, stroke);
}

void Renderer::drawImage(const ImageResource& image, const ImageParams& params) {
  impl_->drawImage(image, params);
}

void Renderer::drawBitmap(const RendererBitmap& bitmap, const ImageParams& params) {
  // Forward so the backend's zero-copy premultiplied path is reachable —
  // falling through to the base default here would convert through the
  // unpremultiplied ImageResource contract on every compositor blit.
  impl_->drawBitmap(bitmap, params);
}

bool Renderer::drawTextureSnapshot(const RendererTextureSnapshot& texture, const Box2d& targetRect,
                                   double opacity, bool pixelated) {
  return impl_->drawTextureSnapshot(texture, targetRect, opacity, pixelated);
}

void Renderer::drawText(Registry& registry, const components::ComputedTextComponent& text,
                        const TextParams& params) {
  impl_->drawText(registry, text, params);
}

RendererBitmap Renderer::takeSnapshot() const {
  return impl_->takeSnapshot();
}

std::shared_ptr<const RendererTextureSnapshot> Renderer::takeTextureSnapshot() {
  return impl_->takeTextureSnapshot();
}

bool Renderer::requiresTextureSnapshotPresentation() const {
  return impl_->requiresTextureSnapshotPresentation();
}

std::unique_ptr<RendererInterface> Renderer::createOffscreenInstance() const {
  return impl_->createOffscreenInstance();
}

bool Renderer::save(const char* filename) {
  const RendererBitmap snapshot = takeSnapshot();
  if (snapshot.empty()) {
    return false;
  }

  const std::size_t strideInPixels = snapshot.rowBytes / 4u;
  return RendererImageIO::writeRgbaPixelsToPngFile(filename, snapshot.pixels, snapshot.dimensions.x,
                                                   snapshot.dimensions.y, strideInPixels);
}

int Renderer::width() const {
  return impl_->width();
}

int Renderer::height() const {
  return impl_->height();
}

}  // namespace donner::svg
