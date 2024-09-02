#include "donner/svg/components/filter/FilterEffect.h"

namespace donner::svg {

bool FilterEffect::operator==(const FilterEffect& other) const {
  return value == other.value;
}

/// Ostream output operator.
std::ostream& operator<<(std::ostream& os, const FilterEffect& paint) {
  os << "FilterEffect(";
  if (paint.is<FilterEffect::None>()) {
    os << "none";
  } else if (paint.is<FilterEffect::Blur>()) {
    os << "blur(" << paint.get<FilterEffect::Blur>().stdDeviationX << " "
       << paint.get<FilterEffect::Blur>().stdDeviationY << ")";
  } else {
    const FilterEffect::ElementReference& ref = paint.get<FilterEffect::ElementReference>();
    os << "url(" << ref.reference.href << ")";
  }
  os << ")";
  return os;
}

}  // namespace donner::svg
