#include "donner/svg/DonnerController.h"

#include <unordered_set>

#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

std::optional<SVGGraphicsElement> DonnerController::HitTarget(Registry& registry, Entity hit) {
  const Entity target = components::RenderingContext::EventTargetEntity(registry, hit);
  if (target == entt::null) {
    return std::nullopt;
  }
  SVGElement element(EntityHandle(registry, target));
  const std::optional<ElementType> type = element.tryType();
  if (!type || !SVGGraphicsElement::IsBaseOf(*type)) {
    return std::nullopt;
  }
  return element.cast<SVGGraphicsElement>();
}

struct DonnerController::HitTestSnapshot::Impl {
  std::optional<std::vector<components::RenderingContext::HitRecord>> records;
  std::vector<SVGGraphicsElement> targets;
};

bool DonnerController::HitTestSnapshot::complete() const {
  return impl_->records.has_value();
}

std::optional<SVGGraphicsElement> DonnerController::HitTestSnapshot::findIntersecting(
    const Vector2d& point) const {
  if (!impl_->records) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < impl_->records->size(); ++i) {
    const auto& record = (*impl_->records)[i];
    if (record.geometry.contains(point, &record.path)) {
      return impl_->targets[i];
    }
  }
  return std::nullopt;
}

std::shared_ptr<const DonnerController::HitTestSnapshot> DonnerController::captureHitTest() {
  const auto access = document_.writeAccess();
  Registry& registry = access.registry();
  auto impl = std::make_shared<HitTestSnapshot::Impl>();
  auto records = components::RenderingContext(registry).captureHitGeometry();
  if (records) {
    impl->records.emplace();
    impl->records->reserve(records->size());
    impl->targets.reserve(records->size());
    for (auto& record : *records) {
      if (std::optional<SVGGraphicsElement> target = HitTarget(registry, record.entity)) {
        impl->records->push_back(std::move(record));
        impl->targets.push_back(std::move(*target));
      }
    }
  }
  return std::shared_ptr<const HitTestSnapshot>(new HitTestSnapshot(std::move(impl)));
}

DonnerController::DonnerController(SVGDocument document) : document_(std::move(document)) {}

std::optional<SVGGraphicsElement> DonnerController::findIntersecting(const Vector2d& point,
                                                                     HitTestMode mode) {
  DocumentWriteAccess access = document_.writeAccess();
  Registry& registry = access.registry();
  components::RenderingContext context(registry);
  const Entity entity = context.findIntersecting(point, mode);
  if (entity == entt::null) {
    return std::nullopt;
  }
  if (std::optional<SVGGraphicsElement> target = HitTarget(registry, entity)) {
    return target;
  }
  // Like the captured snapshot, a hit without a graphics target does not occlude the hits below.
  for (const Entity below : context.findAllIntersecting(point, mode)) {
    if (std::optional<SVGGraphicsElement> target = HitTarget(registry, below)) {
      return target;
    }
  }
  return std::nullopt;
}

std::vector<SVGGraphicsElement> DonnerController::findAllIntersecting(const Vector2d& point,
                                                                      HitTestMode mode) {
  DocumentWriteAccess access = document_.writeAccess();
  Registry& registry = access.registry();
  const std::vector<Entity> entities =
      components::RenderingContext(registry).findAllIntersecting(point, mode);
  std::vector<SVGGraphicsElement> results;
  results.reserve(entities.size());
  // Several shapes instantiated by one `<use>` report that `<use>` once, at its frontmost hit.
  std::unordered_set<Entity> reported;
  for (const Entity entity : entities) {
    std::optional<SVGGraphicsElement> target = HitTarget(registry, entity);
    if (target && reported.insert(target->entityHandle().entity()).second) {
      results.push_back(std::move(*target));
    }
  }
  return results;
}

std::optional<Cursor> DonnerController::cursorAt(const Vector2d& point) {
  DocumentWriteAccess access = document_.writeAccess();
  Registry& registry = access.registry();
  const Entity entity = components::RenderingContext(registry).findIntersecting(point);
  if (entity == entt::null) {
    return std::nullopt;
  }

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  const components::ComputedStyleComponent& style =
      components::StyleSystem().computeStyle(EntityHandle(registry, entity), disabledSink);
  return style.properties->cursor.get().value();
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
