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

std::optional<DonnerController::LinkHit> DonnerController::hitTestLink(const Vector2d& point) {
  std::optional<SVGGraphicsElement> hit = findIntersecting(point);
  if (!hit) {
    return std::nullopt;
  }

  // Enclosing-<a> semantics: a hit on any descendant of an `<a>` resolves to that `<a>`'s target.
  // Walk the ancestor chain, starting at the hit element itself, to the nearest `<a>` that carries
  // a link target. An `<a>` without an href is not a link and is skipped so an outer linked `<a>`
  // can still match.
  std::optional<SVGElement> current = *hit;
  while (current) {
    if (std::optional<SVGAElement> anchor = current->tryCast<SVGAElement>()) {
      if (std::optional<RcString> href = anchor->href()) {
        return LinkHit{*anchor, std::move(*href), *hit};
      }
    }

    current = current->parentElement();
  }

  return std::nullopt;
}

}  // namespace donner::svg
