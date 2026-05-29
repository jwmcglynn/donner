#include "donner/svg/properties/RxRyProperties.h"

namespace donner::svg {

std::tuple<Lengthd, double> CalculateRadiusMaybeAuto(const Property<Lengthd>& property,
                                                     const Property<Lengthd>& fallbackProperty,
                                                     const Box2d& viewBox,
                                                     const FontMetrics& fontMetrics,
                                                     Lengthd::Extent propertyExtent,
                                                     Lengthd::Extent fallbackExtent) {
  if (property.isSpecified()) {
    const Lengthd value = property.get().value();
    const double pixels = value.toPixels(viewBox, fontMetrics, propertyExtent);

    if (pixels >= 0.0) {
      return std::make_tuple(value, pixels);
    }
  }

  // If there's no property, the field is set to "auto", so try using the other dimension
  // (fallbackProperty).
  if (fallbackProperty.isSpecified()) {
    const Lengthd fallbackValue = fallbackProperty.get().value();
    const double fallbackPixels = fallbackValue.toPixels(viewBox, fontMetrics, fallbackExtent);
    if (fallbackPixels >= 0.0) {
      return std::make_tuple(fallbackValue, fallbackPixels);
    }
  }

  // If no value is specified and the fallback doesn't work, return a radius of zero to disable
  // rendering.
  return std::make_tuple(Lengthd(0, Lengthd::Unit::None), 0.0);
}

}  // namespace donner::svg
