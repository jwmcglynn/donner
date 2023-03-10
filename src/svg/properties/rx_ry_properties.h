#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

/**
 * For elements with two radius properties, such as \ref xml_rect or \ref xml_ellipse which provide
 * `rx` and `ry` properties, compute the current value of `rx` or `ry`, while respecting the "auto"
 * identifier and handling negative values.
 *
 * @param property The property to compute, the storage for either `rx` or `ry`.
 * @param fallbackProperty The other property to use if the first one is "auto".
 * @param viewbox The viewbox to use for computing the length.
 * @param fontMetrics The font metrics to use for computing the length.
 * @return Tuple containing the resolved length, and the converted length to pixels.
 */
std::tuple<Lengthd, double> CalculateRadiusMaybeAuto(const Property<Lengthd>& property,
                                                     const Property<Lengthd>& fallbackProperty,
                                                     const Boxd& viewbox,
                                                     const FontMetrics& fontMetrics);

}  // namespace donner::svg
