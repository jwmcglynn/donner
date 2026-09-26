#include "donner/svg/SVGGraphicsElement.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"

namespace donner::svg {

namespace {

bool FiniteTransform(const Transform2d& transform) {
  return std::ranges::all_of(transform.data, [](double value) { return std::isfinite(value); });
}

bool FiniteBox(const Box2d& box) {
  return std::isfinite(box.topLeft.x) && std::isfinite(box.topLeft.y) &&
         std::isfinite(box.bottomRight.x) && std::isfinite(box.bottomRight.y) && !box.isEmpty();
}

bool FinitePathPoints(const Path& path) {
  return std::ranges::all_of(path.points(), [](const Vector2d& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
  });
}

bool SourceWithinOutlineBudget(const Path& path, std::size_t maxVerbs, std::size_t maxPoints,
                               std::size_t maxBytes) {
  const std::optional<std::size_t> bytes = path.retainedBytes();
  return !path.empty() && bytes.has_value() && path.verbCount() <= maxVerbs &&
         path.points().size() <= maxPoints && *bytes <= maxBytes && FinitePathPoints(path);
}

std::optional<Transform2d> ClipUnitsTransform(
    const SVGGraphicsElement& owner, Registry& registry,
    const components::RenderingInstanceComponent& instance) {
  if (instance.clipPath->units == ClipPathUnits::UserSpaceOnUse) {
    return Transform2d();
  }
  if (instance.clipPath->units != ClipPathUnits::ObjectBoundingBox ||
      !owner.isa<SVGGeometryElement>()) {
    return std::nullopt;
  }
  // The renderer uses the owner's local object box. A path-backed shape has an exact bounded
  // box; group/text boxes can require a descendant/font traversal and are not previewed.
  const auto* shape = instance.dataHandle(registry).try_get<components::ComputedPathComponent>();
  if (shape == nullptr) {
    return std::nullopt;
  }
  const Box2d bounds = shape->spline.bounds();
  if (!FiniteBox(bounds)) {
    return std::nullopt;
  }
  return Transform2d::Scale(bounds.size()) * Transform2d::Translate(bounds.topLeft);
}

struct SimpleClipSource {
  const Path* path = nullptr;
  Transform2d unitsTransform;
  Transform2d parentFromEntity;
};

std::optional<SimpleClipSource> ResolveSimpleClipSource(
    const SVGGraphicsElement& owner, Registry& registry,
    const components::RenderingInstanceComponent& instance, std::size_t maxVerbs,
    std::size_t maxPoints, std::size_t maxBytes) {
  if (!instance.clipPath.has_value() || !instance.clipPath->valid()) {
    return std::nullopt;
  }
  const auto* computed =
      instance.styleHandle(registry).try_get<components::ComputedClipPathsComponent>();
  if (computed == nullptr || computed->clipPaths.size() != 1u ||
      computed->clipPaths.front().layer != 0) {
    return std::nullopt;
  }
  const Path& path = computed->clipPaths.front().path;
  const std::optional<Transform2d> units = ClipUnitsTransform(owner, registry, instance);
  if (!SourceWithinOutlineBudget(path, maxVerbs, maxPoints, maxBytes) || !units.has_value()) {
    return std::nullopt;
  }
  return SimpleClipSource{&path, *units, computed->clipPaths.front().parentFromEntity};
}

std::optional<Path> TransformClipOutline(const Path& source, const Transform2d& documentFromClip,
                                         std::size_t maxBytes) {
  if (!FiniteTransform(documentFromClip) ||
      !std::ranges::all_of(source.points(), [&](const Vector2d& point) {
        const Vector2d transformed = documentFromClip.transformPosition(point);
        return std::isfinite(transformed.x) && std::isfinite(transformed.y);
      })) {
    return std::nullopt;
  }
  Path documentPath = source.transformed(documentFromClip);
  const std::optional<std::size_t> bytes = documentPath.retainedBytes();
  if (!bytes.has_value() || *bytes > maxBytes || !FiniteBox(documentPath.bounds())) {
    return std::nullopt;
  }
  return documentPath;
}

}  // namespace

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transform2d SVGGraphicsElement::transform() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  return components::LayoutSystem().getRawEntityFromParentTransform(handle_);
}

void SVGGraphicsElement::setTransform(const Transform2d& transform) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, transform);
}

Transform2d SVGGraphicsElement::elementFromWorld() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  return components::LayoutSystem().getEntityFromWorldTransform(handle_);
}

std::optional<Path> SVGGraphicsElement::resolvedSimpleClipPathOutline(std::size_t maxVerbs,
                                                                      std::size_t maxPoints,
                                                                      std::size_t maxBytes,
                                                                      bool* hasClipPath) const {
  if (hasClipPath != nullptr) {
    *hasClipPath = false;
  }
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  Registry& registry = *handle_.registry();
  const auto* instance = registry.try_get<components::RenderingInstanceComponent>(handle_.entity());
  if (instance == nullptr || !instance->visible) {
    return std::nullopt;
  }
  if (hasClipPath != nullptr) {
    *hasClipPath = instance->clipPath.has_value();
  }
  const std::optional<SimpleClipSource> source =
      ResolveSimpleClipSource(*this, registry, *instance, maxVerbs, maxPoints, maxBytes);
  if (!source.has_value()) {
    return std::nullopt;
  }
  const Transform2d documentFromClip =
      source->unitsTransform * source->parentFromEntity *
      components::LayoutSystem().getEntityFromWorldTransform(handle_);
  return TransformClipOutline(*source->path, documentFromClip, maxBytes);
}

}  // namespace donner::svg
