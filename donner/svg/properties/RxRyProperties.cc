#include "donner/svg/properties/RxRyProperties.h"

namespace donner::svg {

std::tuple<Lengthd, double> CalculateRadiusMaybeAuto(const Property<Lengthd>& property,
                                                     const Property<Lengthd>& fallbackProperty,
                                                     const Boxd& viewBox,
                                                     const FontMetrics& fontMetrics) {
  if (property.hasValue()) {
    const Lengthd value = property.getRequired();
    const double pixels = value.toPixels(viewBox, fontMetrics);

    if (pixels >= 0.0) {
      return std::make_tuple(value, pixels);
    }
  }

  // If there's no property, the field is set to "auto", so try using the other dimension
  // (fallbackProperty).
  if (fallbackProperty.hasValue()) {
    const Lengthd fallbackValue = fallbackProperty.getRequired();
    const double fallbackPixels = fallbackValue.toPixels(viewBox, fontMetrics);
    if (fallbackPixels >= 0.0) {
      return std::make_tuple(fallbackValue, fallbackPixels);
    }
  }

  // If no value is specified and the fallback doesn't work, return a radius of zero to disable
  // rendering.
  return std::make_tuple(Lengthd(0, Lengthd::Unit::None), 0.0);
}

}  // namespace donner::svg
