#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Length.h"
#include "donner/css/Color.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/core/Gradient.h"

namespace donner::svg {

/// Fully resolved gradient payload that can cross non-SVG package boundaries.
struct ResolvedGradientData {
  enum class Kind : uint8_t { kLinear = 0, kRadial = 1 };

  Kind kind = Kind::kLinear;

  GradientUnits units = GradientUnits::Default;
  GradientSpreadMethod spreadMethod = GradientSpreadMethod::Default;
  std::vector<GradientStop> stops;

  Lengthd x1;
  Lengthd y1;
  Lengthd x2;
  Lengthd y2;

  Lengthd cx;
  Lengthd cy;
  Lengthd r;
  std::optional<Lengthd> fx;
  std::optional<Lengthd> fy;
  Lengthd fr;

  std::optional<css::Color> fallback;
};

/**
 * Flatten a resolved paint server if it points at a materialized linear or radial gradient.
 *
 * @param paint The resolved paint server to inspect.
 * @return Gradient payload on success, or `std::nullopt` if `paint` is not a supported gradient.
 */
[[nodiscard]] std::optional<ResolvedGradientData> FlattenResolvedGradient(
    const components::ResolvedPaintServer& paint);

/**
 * Materialize a resolved gradient payload into a fresh ECS paint server entity.
 *
 * @param registry Registry that will own the temporary paint server entity.
 * @param gradient Gradient payload to materialize.
 * @return A resolved paint server referencing the newly created entity.
 */
components::ResolvedPaintServer MaterializeResolvedGradient(Registry& registry,
                                                            const ResolvedGradientData& gradient);

}  // namespace donner::svg
