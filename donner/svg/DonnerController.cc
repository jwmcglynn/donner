#include "donner/svg/DonnerController.h"

#include "donner/base/Utils.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

DonnerController::DonnerController(SVGDocument document) : document_(std::move(document)) {}

std::optional<SVGGeometryElement> DonnerController::findIntersecting(const Vector2d& point) {
  Entity entity = components::RenderingContext(document_.registry()).findIntersecting(point);
  if (entity != entt::null) {
    SVGElement element(EntityHandle(document_.registry(), entity));
    UTILS_RELEASE_ASSERT(element.isa<SVGGeometryElement>());
    return element.cast<SVGGeometryElement>();
  }

  return std::nullopt;
}

}  // namespace donner::svg
