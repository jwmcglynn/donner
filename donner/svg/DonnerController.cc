#include "donner/svg/DonnerController.h"

#include "donner/base/Utils.h"
#include "donner/svg/components/EventListenersComponent.h"
#include "donner/svg/components/EventSystem.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

/**
 * Constructor to create the controller from a given \ref SVGDocument. Allows querying and
 * controlling the SVG contents beyond what the DOM provides.
 */
DonnerController::DonnerController(SVGDocument document) : document_(std::move(document)) {}

/**
 * Finds the first element that intersects the given point.
 *
 * @param point Pointer position to find the intersecting element for
 */
std::optional<SVGGeometryElement> DonnerController::findIntersecting(const Vector2d& point) {
  Entity entity = components::RenderingContext(document_.registry()).findIntersecting(point);
  if (entity != entt::null) {
    SVGElement element(EntityHandle(document_.registry(), entity));
    UTILS_RELEASE_ASSERT(element.isa<SVGGeometryElement>());
    return element.cast<SVGGeometryElement>();
  }

  return std::nullopt;
}

std::vector<SVGElement> DonnerController::findAllIntersecting(const Vector2d& point) {
  std::vector<Entity> entities =
      components::RenderingContext(document_.registry()).findAllIntersecting(point);

  std::vector<SVGElement> results;
  results.reserve(entities.size());
  for (Entity entity : entities) {
    results.push_back(SVGElement(EntityHandle(document_.registry(), entity)));
  }
  return results;
}

std::vector<SVGElement> DonnerController::findIntersectingRect(const Boxd& rect) {
  std::vector<Entity> entities =
      components::RenderingContext(document_.registry()).findIntersectingRect(rect);

  std::vector<SVGElement> results;
  results.reserve(entities.size());
  for (Entity entity : entities) {
    results.push_back(SVGElement(EntityHandle(document_.registry(), entity)));
  }
  return results;
}

std::optional<Boxd> DonnerController::getWorldBounds(SVGElement element) {
  // Ensure the render tree is instantiated so transforms are computed.
  components::RenderingContext(document_.registry()).instantiateRenderTree(false, nullptr);

  return components::ShapeSystem().getShapeWorldBounds(element.entityHandle());
}

std::optional<SVGElement> DonnerController::dispatchEvent(Event& event) {
  // If no target is set, hit-test to find one.
  if (event.target == entt::null) {
    event.target =
        components::RenderingContext(document_.registry()).findIntersecting(event.documentPosition);
  }

  if (event.target == entt::null) {
    return std::nullopt;
  }

  components::EventSystem().dispatch(document_.registry(), event);

  // Resolve the target entity to an SVGElement.
  return SVGElement(EntityHandle(document_.registry(), event.target));
}

void DonnerController::updateHover(const Vector2d& point) {
  Entity hitEntity =
      components::RenderingContext(document_.registry()).findIntersecting(point);

  components::EventSystem().updateHover(document_.registry(), hitEntity, point);
}

std::optional<SVGElement> DonnerController::hoveredElement() {
  const auto* pointerState =
      document_.registry().ctx().find<components::PointerStateComponent>();
  if (!pointerState || pointerState->hoveredEntity == entt::null) {
    return std::nullopt;
  }

  return SVGElement(EntityHandle(document_.registry(), pointerState->hoveredEntity));
}

CursorType DonnerController::getCursorAt(const Vector2d& point) {
  Entity entity =
      components::RenderingContext(document_.registry()).findIntersecting(point);
  if (entity == entt::null) {
    return CursorType::Auto;
  }

  const auto& style =
      components::StyleSystem().computeStyle(EntityHandle(document_.registry(), entity), nullptr);
  return style.properties->cursor.getRequired();
}

}  // namespace donner::svg
