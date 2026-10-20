#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Length.h"
#include "donner/css/Color.h"

namespace donner::svg::components {

/// Whether a CSS scalar stack needs the final instance's parent cascade.
/// @param attribute Presentation property name.
/// @return True for the supported opacity and miter-limit scalar properties.
bool IsContextualScalarAttribute(std::string_view attribute);

/// Whether a paint/color stack needs the final instance's cascade.
/// @param attribute Presentation property name.
bool IsContextualPaintAttribute(std::string_view attribute);

/// Serialize an escaped, quoted CSS URL within the animation output envelope.
/// @param href Raw URL. The caller admits copying/escaping work before calling.
/// @return Owned CSS representation, or nullopt when it exceeds the output envelope.
std::optional<std::string> SerializeAnimationUrl(std::string_view href);

/// Context required to resolve relative units and currentColor for an animated value.
struct AnimationValueContext {
  Box2d viewBox{{0, 0}, {300, 150}};  ///< Current SVG viewport in user units.
  bool angleValue = false;            ///< Marker orientation operands use SVG angle units.
  bool referenceValue = false;        ///< References are discrete and non-additive.
  /// Zero accepts general values; otherwise only this many dimensionless operands are allowed.
  std::size_t maximumNumbers = 0;
  bool percentageNumbers =
      false;                  ///< Percentages represent fractions of one, not viewport lengths.
  bool numberPair = false;    ///< Repeat one coordinate for number-optional-number values.
  bool integerValue = false;  ///< Round interpolated numeric values to the nearest integer.
  bool linearRgb = false;     ///< Interpolate colors in linear-light RGB when requested.
  /// Map mixed-unit filter lengths to user-space pixels while keeping additive offsets origin-free.
  bool mappedLengths = false;
  bool lengthValuesAreOffsets = false;  ///< Sample operands are displacements, not positions.
  double unitlessScale = 1;             ///< User units represented by one unitless coordinate.
  double unitlessOrigin = 0;            ///< Origin added only to unitless positions.
  double percentageOrigin = 0;          ///< Origin added only to percentage positions.
  FontMetrics fontMetrics;              ///< Font and viewport unit metrics.
  Lengthd::Extent extent = Lengthd::Extent::Mixed;   ///< Percentage reference axis.
  css::RGBA currentColor = css::RGBA::RGB(0, 0, 0);  ///< Resolved currentColor.
};

/// Parse a finite length list within the animation numeric-value limit.
/// @param input SVG comma/space-separated lengths, with optional units.
/// @return Parsed values, or nullopt for an empty, malformed, or excessive list.
std::optional<std::vector<Lengthd>> ParseAnimationLengths(std::string_view input);

/// Validate a source operand against the target's numeric grammar before interpolation.
/// @param input Authored or underlying operand. @param context Target numeric restrictions.
/// @return Whether the operand is valid, or the context accepts general values.
bool ValidAnimationNumericOperand(std::string_view input, const AnimationValueContext& context);

/// Parse one animated length, including axis-appropriate refX/refY reference keywords.
/// @param attribute Animated field name. @param value Sampled value.
/// @return One finite length, or nullopt for an invalid field value.
std::optional<Lengthd> ParseAnimationLength(std::string_view attribute, std::string_view value);

/** Combine compatible numeric, length-list, or color values with linear weights.
 * @param first First value.
 * @param second Second value.
 * @param firstWeight Multiplier of the first value.
 * @param secondWeight Multiplier of the second value.
 * @param context Relative unit and color context.
 * @param secondIsOffset Treat the second length operand as an additive displacement.
 * @return Serialized result, or nullopt for incompatible/discrete values.
 */
std::optional<std::string> CombineAnimationValues(std::string_view first, std::string_view second,
                                                  double firstWeight, double secondWeight,
                                                  const AnimationValueContext& context,
                                                  bool secondIsOffset = false);

/** Compute the paced distance between compatible values.
 * @param first First value.
 * @param second Second value.
 * @param context Relative unit and color context.
 * @return Distance, or nullopt when no distance function is defined.
 */
std::optional<double> AnimationValueDistance(std::string_view first, std::string_view second,
                                             const AnimationValueContext& context);

}  // namespace donner::svg::components
