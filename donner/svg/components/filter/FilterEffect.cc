#include "donner/svg/components/filter/FilterEffect.h"

namespace donner::svg {

bool FilterEffect::operator==(const FilterEffect& other) const {
  return value == other.value;
}

/// Ostream output operator.
std::ostream& operator<<(std::ostream& os, const FilterEffect& filter) {
  os << "FilterEffect(";
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, FilterEffect::None>) {
          os << "none";
        } else if constexpr (std::is_same_v<T, FilterEffect::Blur>) {
          os << "blur(" << e.stdDeviationX << " " << e.stdDeviationY << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::ElementReference>) {
          os << "url(" << e.reference.href << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::HueRotate>) {
          os << "hue-rotate(" << e.angleDegrees << "deg)";
        } else if constexpr (std::is_same_v<T, FilterEffect::Brightness>) {
          os << "brightness(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::Contrast>) {
          os << "contrast(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::Grayscale>) {
          os << "grayscale(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::Invert>) {
          os << "invert(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::FilterOpacity>) {
          os << "opacity(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::Saturate>) {
          os << "saturate(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::Sepia>) {
          os << "sepia(" << e.amount << ")";
        } else if constexpr (std::is_same_v<T, FilterEffect::DropShadow>) {
          os << "drop-shadow(" << e.offsetX.value << " " << e.offsetY.value << " "
             << e.stdDeviation.value << ")";
        }
      },
      filter.value);
  os << ")";
  return os;
}

std::ostream& operator<<(std::ostream& os, const std::vector<FilterEffect>& filters) {
  os << "[";
  for (size_t i = 0; i < filters.size(); ++i) {
    if (i > 0) {
      os << ", ";
    }
    os << filters[i];
  }
  os << "]";
  return os;
}

}  // namespace donner::svg
