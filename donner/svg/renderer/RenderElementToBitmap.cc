#include "donner/svg/renderer/RenderElementToBitmap.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

namespace {

/// Collect the data entities of @p element and every descendant, via DOM
/// traversal. These are the source entities whose rendering instances make up
/// the element's isolated subtree. Bounded against pathological nesting.
///
/// @param element Subtree root.
/// @param out Receives the data entity of @p element and each descendant.
void CollectSubtreeEntities(const SVGElement& element, std::unordered_set<Entity>& out) {
  out.insert(element.entityHandle().entity());
  for (std::optional<SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    CollectSubtreeEntities(*child, out);
  }
}

/// The rendered span of an element's subtree: the inclusive draw-order range
/// `[firstEntity, lastEntity]` (as the entities `RenderingInstanceView` yields)
/// plus the union of the descendants' geometry world-bounds.
struct SubtreeRenderSpan {
  Entity firstEntity = entt::null;   ///< Lowest-draw-order rendering instance.
  Entity lastEntity = entt::null;    ///< Highest-draw-order rendering instance.
  std::optional<Box2d> worldBounds;  ///< Document-space bounds of painted geometry.
};

/// Resolve the rendering span for the subtree rooted at the entities in
/// @p subtreeEntities. Walks every prepared `RenderingInstanceComponent`,
/// selecting those whose `dataEntity` is in the subtree, and tracks the
/// lowest/highest draw-order instances (the inclusive range `drawEntityRange`
/// needs) plus the union of their geometry bounds (in document/world space).
///
/// Driving the range from the DOM subtree — rather than the host instance's
/// `subtreeInfo` — handles `<g>` groups, whose rendering instance carries no
/// `subtreeInfo`, by spanning all of the group's drawn descendants. Bounds come
/// from `ComputedPathComponent::localBounds()` transformed to world space, so
/// they are true document coordinates (not clamped to any viewport, unlike
/// `RendererDriver::computeEntityRangeBounds`, which intersects with its render
/// viewport and would crop a shape that lies outside the small thumbnail box).
///
/// @param registry Prepared render-tree registry.
/// @param subtreeEntities Data entities of the element and its descendants.
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
    // Use `spline.bounds()` (computed from the actual geometry) rather than
    // `localBounds()`, whose memoized cache is not reliably populated at this
    // point for converted shapes like `<circle>`/`<ellipse>` (it returns an
    // empty box, collapsing the thumbnail's fit math). `spline.bounds()` walks
    // the path once; thumbnails are produced off the hot path so the cost is
    // irrelevant.
    const Box2d local = path->spline.bounds();
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

RendererBitmap RenderElementToBitmap(SVGElement element, Vector2i sizePx) {
  if (sizePx.x <= 0 || sizePx.y <= 0) {
    return RendererBitmap{};
  }

  SVGDocument document = element.ownerDocument();
  DocumentWriteAccess access = document.writeAccess();

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warnings);

  Registry& registry = document.registry();

  // Gather the element + descendant data entities, then resolve the inclusive
  // draw-order entity range and the document-space geometry bounds for that
  // subtree. An empty span (no boundable geometry: empty group, text, image)
  // yields an empty bitmap so the caller falls back to its swatch.
  std::unordered_set<Entity> subtreeEntities;
  CollectSubtreeEntities(element, subtreeEntities);
  const SubtreeRenderSpan span = ResolveSubtreeRenderSpan(registry, subtreeEntities);
  if (span.firstEntity == entt::null || span.lastEntity == entt::null ||
      !span.worldBounds.has_value() || span.worldBounds->isEmpty()) {
    return RendererBitmap{};
  }
  const Box2d worldBounds = *span.worldBounds;

  RenderViewport viewport;
  viewport.size = Vector2d(sizePx.x, sizePx.y);
  viewport.devicePixelRatio = 1.0;

  RendererTinySkia renderer;
  RendererDriver driver(renderer);

  // Inset so a centered shape does not touch the bitmap edge (gives strokes and
  // round shapes a little breathing room in the preview cell).
  constexpr double kInsetPx = 1.0;
  const double availableW = std::max(1.0, viewport.size.x - kInsetPx * 2.0);
  const double availableH = std::max(1.0, viewport.size.y - kInsetPx * 2.0);

  const double boundsW = worldBounds.width();
  const double boundsH = worldBounds.height();
  const double scale = std::min(availableW / boundsW, availableH / boundsH);

  // Center the scaled bounds within the target box.
  const double scaledW = boundsW * scale;
  const double scaledH = boundsH * scale;
  const Vector2d offset((viewport.size.x - scaledW) * 0.5, (viewport.size.y - scaledH) * 0.5);

  // surfaceFromCanvas maps world/document coords into the centered, fitted
  // device cell: first translate the bounds' top-left to the origin, then scale
  // to fit, then translate by the centering offset. `Transform2d` position
  // mapping composes left-first ("apply A, then B"), so the factors read source
  // -first: `Translate(-topLeft) * Scale(scale) * Translate(offset)`, giving
  // translate = offset - topLeft*scale (e.g. (1,1) - (10,10)*0.275 =
  // (-1.75,-1.75)). The opposite order bakes `offset - topLeft` *before*
  // scaling, pushing a shape whose bounds don't start at the origin off the
  // top-left of the cell (verified: a `<circle>` thumbnail lands in the corner).
  const Transform2d surfaceFromCanvas = Transform2d::Translate(-worldBounds.topLeft) *
                                        Transform2d::Scale(scale) * Transform2d::Translate(offset);

  driver.drawEntityRange(registry, span.firstEntity, span.lastEntity, viewport, surfaceFromCanvas);
  return driver.takeSnapshot();
}

}  // namespace donner::svg
