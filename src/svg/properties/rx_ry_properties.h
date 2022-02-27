#pragma once

#include "src/base/length.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

/**
 * For rx/ry properties, <rect> or <ellipse>, handles computing the current value of rx or ry, while
 * respecting the "auto" identifier and handling negative values.
 *
 * @param property The property to compute.
 * @param fallbackProperty The other property to use if the first one is "auto".
 * @param viewbox The viewbox to use for computing the length.
 * @param fontMetrics The font metrics to use for computing the length.
 * @return std::tuple<Lengthd, double> containing the resolved length, and the converted length to
 *  pixels.
 */
std::tuple<Lengthd, double> CalculateRadiusMaybeAuto(const Property<Lengthd>& property,
                                                     const Property<Lengthd>& fallbackProperty,
                                                     const Boxd& viewbox,
                                                     const FontMetrics& fontMetrics);

}  // namespace donner::svg
