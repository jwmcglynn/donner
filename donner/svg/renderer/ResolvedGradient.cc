#include "donner/svg/renderer/ResolvedGradient.h"

#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"

namespace donner::svg {

std::optional<ResolvedGradientData> FlattenResolvedGradient(
    const components::ResolvedPaintServer& paint) {
  const auto* ref = std::get_if<components::PaintResolvedReference>(&paint);
  if (ref == nullptr) {
    return std::nullopt;
  }

  const auto& handle = ref->reference.handle;
  const auto* gradient = handle.try_get<components::ComputedGradientComponent>();
  if (gradient == nullptr || !gradient->initialized) {
    return std::nullopt;
  }

  ResolvedGradientData out;
  out.units = gradient->gradientUnits;
  out.spreadMethod = gradient->spreadMethod;
  out.stops = gradient->stops;
  out.fallback = ref->fallback;

  if (const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>()) {
    out.kind = ResolvedGradientData::Kind::kLinear;
    out.x1 = linear->x1;
    out.y1 = linear->y1;
    out.x2 = linear->x2;
    out.y2 = linear->y2;
    return out;
  }

  if (const auto* radial = handle.try_get<components::ComputedRadialGradientComponent>()) {
    out.kind = ResolvedGradientData::Kind::kRadial;
    out.cx = radial->cx;
    out.cy = radial->cy;
    out.r = radial->r;
    out.fx = radial->fx;
    out.fy = radial->fy;
    out.fr = radial->fr;
    return out;
  }

  return std::nullopt;
}

components::ResolvedPaintServer MaterializeResolvedGradient(Registry& registry,
                                                            const ResolvedGradientData& gradient) {
  const Entity entity = registry.create();
  auto& computed = registry.emplace<components::ComputedGradientComponent>(entity);
  computed.initialized = true;
  computed.gradientUnits = gradient.units;
  computed.spreadMethod = gradient.spreadMethod;
  computed.stops = gradient.stops;

  if (gradient.kind == ResolvedGradientData::Kind::kLinear) {
    auto& linear = registry.emplace<components::ComputedLinearGradientComponent>(entity);
    linear.x1 = gradient.x1;
    linear.y1 = gradient.y1;
    linear.x2 = gradient.x2;
    linear.y2 = gradient.y2;
  } else {
    auto& radial = registry.emplace<components::ComputedRadialGradientComponent>(entity);
    radial.cx = gradient.cx;
    radial.cy = gradient.cy;
    radial.r = gradient.r;
    radial.fx = gradient.fx;
    radial.fy = gradient.fy;
    radial.fr = gradient.fr;
  }

  components::PaintResolvedReference out;
  out.reference = ResolvedReference{EntityHandle(registry, entity)};
  out.fallback = gradient.fallback;
  return out;
}

}  // namespace donner::svg
