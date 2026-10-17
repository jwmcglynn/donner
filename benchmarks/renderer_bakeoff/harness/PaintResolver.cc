#include "harness/PaintResolver.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"

namespace donner::bakeoff {

namespace {

using svg::components::ComputedGradientComponent;
using svg::components::ComputedLinearGradientComponent;
using svg::components::ComputedLocalTransformComponent;
using svg::components::ComputedRadialGradientComponent;
using svg::components::PaintResolvedReference;

const Box2d kUnitBounds(Vector2d::Zero(), Vector2d(1, 1));

Lengthd ToPercent(Lengthd value, bool numbersArePercent) {
  if (numbersArePercent && value.unit == Lengthd::Unit::None) {
    value.value *= 100.0;
    value.unit = Lengthd::Unit::Percent;
  }
  return value;
}

double ResolveCoord(Lengthd value, const Box2d& bounds, bool numbersArePercent) {
  return ToPercent(value, numbersArePercent).toPixels(bounds, FontMetrics());
}

Vector2d ResolveCoords(Lengthd x, Lengthd y, const Box2d& bounds, bool numbersArePercent) {
  return Vector2d(
      ToPercent(x, numbersArePercent).toPixels(bounds, FontMetrics(), Lengthd::Extent::X),
      ToPercent(y, numbersArePercent).toPixels(bounds, FontMetrics(), Lengthd::Extent::Y));
}

Transform2d GradientTransform(const ComputedLocalTransformComponent* transform,
                              const Box2d& bounds) {
  if (transform == nullptr) {
    return Transform2d();
  }
  const Vector2d origin = transform->transformOrigin;
  const Transform2d parentFromEntity = transform->rawCssTransform.compute(bounds, FontMetrics());
  // Same transform-origin pivot order as shape transforms: translate out, apply, translate back.
  return Transform2d::Translate(-origin) * parentFromEntity * Transform2d::Translate(origin);
}

SpreadMode ToSpread(svg::GradientSpreadMethod method) {
  switch (method) {
    case svg::GradientSpreadMethod::Reflect: return SpreadMode::Reflect;
    case svg::GradientSpreadMethod::Repeat: return SpreadMode::Repeat;
    case svg::GradientSpreadMethod::Pad: return SpreadMode::Pad;
  }
  return SpreadMode::Pad;
}

std::optional<PaintDesc> ResolveGradient(const PaintResolvedReference& ref, float opacity,
                                         const css::RGBA& currentColor, const Box2d& pathBounds,
                                         const Box2d& viewBox) {
  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }
  const auto* computed = handle.try_get<ComputedGradientComponent>();
  if (computed == nullptr || !computed->initialized) {
    return std::nullopt;
  }

  const bool objectBoundingBox = computed->gradientUnits == svg::GradientUnits::ObjectBoundingBox;
  const Box2d& referenceBounds = ref.contextRemap ? ref.contextRemap->contextBounds : pathBounds;
  constexpr double kDegenerateTolerance = 1e-6;
  if (objectBoundingBox && (NearZero(referenceBounds.width(), kDegenerateTolerance) ||
                            NearZero(referenceBounds.height(), kDegenerateTolerance))) {
    return std::nullopt;
  }

  GradientDesc gradient;
  if (objectBoundingBox) {
    gradient.pathFromGradient =
        GradientTransform(handle.try_get<ComputedLocalTransformComponent>(), kUnitBounds) *
        Transform2d::Scale(referenceBounds.size()) *
        Transform2d::Translate(referenceBounds.topLeft);
  } else {
    gradient.pathFromGradient =
        GradientTransform(handle.try_get<ComputedLocalTransformComponent>(), viewBox);
  }
  if (ref.contextRemap) {
    gradient.pathFromGradient =
        gradient.pathFromGradient * ref.contextRemap->entityFromContextTransform;
  }

  for (const svg::GradientStop& stop : computed->stops) {
    gradient.stops.push_back(
        {std::clamp(stop.offset, 0.0f, 1.0f),
         ColorF::FromRgba(stop.color.resolve(currentColor, stop.opacity * opacity))});
  }
  if (gradient.stops.empty()) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < gradient.stops.size(); ++i) {
    gradient.stops[i].offset = std::max(gradient.stops[i].offset, gradient.stops[i - 1].offset);
  }
  if (gradient.stops.size() == 1) {
    PaintDesc solid;
    solid.kind = PaintDesc::Kind::Solid;
    solid.color = gradient.stops.front().color;
    return solid;
  }

  gradient.spread = ToSpread(computed->spreadMethod);
  const Box2d& bounds = objectBoundingBox ? kUnitBounds : viewBox;

  if (const auto* linear = handle.try_get<ComputedLinearGradientComponent>()) {
    gradient.type = GradientDesc::Type::Linear;
    gradient.start = ResolveCoords(linear->x1, linear->y1, bounds, objectBoundingBox);
    gradient.end = ResolveCoords(linear->x2, linear->y2, bounds, objectBoundingBox);
    if (gradient.start == gradient.end) {
      PaintDesc solid;
      solid.kind = PaintDesc::Kind::Solid;
      solid.color = gradient.stops.back().color;
      return solid;
    }
  } else if (const auto* radial = handle.try_get<ComputedRadialGradientComponent>()) {
    gradient.type = GradientDesc::Type::Radial;
    gradient.radius = ResolveCoord(radial->r, bounds, objectBoundingBox);
    gradient.center = ResolveCoords(radial->cx, radial->cy, bounds, objectBoundingBox);
    gradient.focalRadius = ResolveCoord(radial->fr, bounds, objectBoundingBox);
    gradient.focal = ResolveCoords(radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy),
                                   bounds, objectBoundingBox);
    if (NearZero(gradient.radius)) {
      PaintDesc solid;
      solid.kind = PaintDesc::Kind::Solid;
      solid.color = gradient.stops.back().color;
      return solid;
    }
    if ((gradient.center - gradient.focal).length() + gradient.radius <= gradient.focalRadius) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }

  PaintDesc result;
  result.kind = PaintDesc::Kind::Gradient;
  result.gradient = std::move(gradient);
  return result;
}

}  // namespace

ColorF ColorF::FromRgba(const css::RGBA& rgba) {
  return ColorF{static_cast<float>(rgba.r) / 255.0f, static_cast<float>(rgba.g) / 255.0f,
                static_cast<float>(rgba.b) / 255.0f, static_cast<float>(rgba.a) / 255.0f};
}

css::RGBA ColorF::toRgba() const {
  const auto channel = [](float value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  };
  return css::RGBA(channel(r), channel(g), channel(b), channel(a));
}

PaintDesc ResolvePaint(const svg::components::ResolvedPaintServer& paint, double opacity,
                       const css::RGBA& currentColor, const Box2d& pathBounds,
                       const Box2d& viewBox) {
  const float opacityF = static_cast<float>(opacity);
  if (const auto* solid = std::get_if<svg::PaintServer::Solid>(&paint)) {
    PaintDesc result;
    result.kind = PaintDesc::Kind::Solid;
    result.color = ColorF::FromRgba(solid->color.resolve(currentColor, opacityF));
    return result;
  }

  if (const auto* ref = std::get_if<PaintResolvedReference>(&paint)) {
    if (std::optional<PaintDesc> gradient =
            ResolveGradient(*ref, opacityF, currentColor, pathBounds, viewBox)) {
      return *gradient;
    }
    if (ref->fallback.has_value()) {
      PaintDesc result;
      result.kind = PaintDesc::Kind::Solid;
      result.color = ColorF::FromRgba(ref->fallback->resolve(currentColor, opacityF));
      return result;
    }
  }

  return PaintDesc{};
}

}  // namespace donner::bakeoff
