#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Length.h"
#include "donner/css/Color.h"

namespace donner::svg::components {

/// Context required to resolve relative units and currentColor for an animated value.
struct AnimationValueContext {
  Box2d viewBox{{0, 0}, {300, 150}};  ///< Current SVG viewport in user units.
  bool referenceValue = false;        ///< References are discrete and non-additive.
  bool numberPair = false;            ///< Repeat one coordinate for number-optional-number values.
  bool integerValue = false;          ///< Round interpolated numeric values to the nearest integer.
  bool linearRgb = false;             ///< Interpolate colors in linear-light RGB when requested.
  FontMetrics fontMetrics;            ///< Font and viewport unit metrics.
  Lengthd::Extent extent = Lengthd::Extent::Mixed;   ///< Percentage reference axis.
  css::RGBA currentColor = css::RGBA::RGB(0, 0, 0);  ///< Resolved currentColor.
};

/// Parse a finite length list within the animation numeric-value limit.
/// @param input SVG comma/space-separated lengths, with optional units.
/// @return Parsed values, or nullopt for an empty, malformed, or excessive list.
std::optional<std::vector<Lengthd>> ParseAnimationLengths(std::string_view input);

/** Combine compatible numeric, length-list, or color values with linear weights.
 * @param first First value.
 * @param second Second value.
 * @param firstWeight Multiplier of the first value.
 * @param secondWeight Multiplier of the second value.
 * @param context Relative unit and color context.
 * @return Serialized result, or nullopt for incompatible/discrete values.
 */
std::optional<std::string> CombineAnimationValues(std::string_view first, std::string_view second,
                                                  double firstWeight, double secondWeight,
                                                  const AnimationValueContext& context);

/** Compute the paced distance between compatible values.
 * @param first First value.
 * @param second Second value.
 * @param context Relative unit and color context.
 * @return Distance, or nullopt when no distance function is defined.
 */
std::optional<double> AnimationValueDistance(std::string_view first, std::string_view second,
                                             const AnimationValueContext& context);

}  // namespace donner::svg::components
