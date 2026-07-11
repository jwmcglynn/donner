#include "donner/svg/DonnerController.h"

#include "donner/base/Utils.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

DonnerController::DonnerController(SVGDocument document) : document_(std::move(document)) {}

std::optional<SVGGraphicsElement> DonnerController::findIntersecting(const Vector2d& point) {
  DocumentWriteAccess access = document_.writeAccess();
  Registry& registry = access.registry();
  Entity entity = components::RenderingContext(registry).findIntersecting(point);
  if (entity != entt::null) {
    SVGElement element(EntityHandle(registry, entity));
    UTILS_RELEASE_ASSERT(element.isa<SVGGraphicsElement>());
    return element.cast<SVGGraphicsElement>();
  }

  return std::nullopt;
}

std::vector<SVGGraphicsElement> DonnerController::findAllIntersecting(const Vector2d& point) {
  DocumentWriteAccess access = document_.writeAccess();
  Registry& registry = access.registry();
  const std::vector<Entity> entities =
      components::RenderingContext(registry).findAllIntersecting(point);
  std::vector<SVGGraphicsElement> results;
  results.reserve(entities.size());
  for (const Entity entity : entities) {
    SVGElement element(EntityHandle(registry, entity));
    UTILS_RELEASE_ASSERT(element.isa<SVGGraphicsElement>());
    results.push_back(element.cast<SVGGraphicsElement>());
  }
  return results;
}

}  // namespace donner::svg
