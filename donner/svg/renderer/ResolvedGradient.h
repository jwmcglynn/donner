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
  /// Geometry represented by this gradient payload.
  enum class Kind : uint8_t {
    kLinear = 0,  ///< Gradient between two endpoints.
    kRadial = 1,  ///< Gradient between focal and outer circles.
  };

  Kind kind = Kind::kLinear;  ///< Geometry selected for this payload.

  GradientUnits units = GradientUnits::Default;  ///< Coordinate system for geometry lengths.
  GradientSpreadMethod spreadMethod = GradientSpreadMethod::Default;  ///< Out-of-range behavior.
  std::vector<GradientStop> stops;  ///< Ordered color stops of the resolved gradient.

  Lengthd x1;  ///< Linear gradient start X coordinate.
  Lengthd y1;  ///< Linear gradient start Y coordinate.
  Lengthd x2;  ///< Linear gradient end X coordinate.
  Lengthd y2;  ///< Linear gradient end Y coordinate.

  Lengthd cx;                 ///< Radial gradient outer-circle center X coordinate.
  Lengthd cy;                 ///< Radial gradient outer-circle center Y coordinate.
  Lengthd r;                  ///< Radial gradient outer-circle radius.
  std::optional<Lengthd> fx;  ///< Optional focal-circle center X coordinate.
  std::optional<Lengthd> fy;  ///< Optional focal-circle center Y coordinate.
  Lengthd fr;                 ///< Focal-circle radius.

  std::optional<css::Color> fallback;  ///< Fallback paint color, if specified.
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
