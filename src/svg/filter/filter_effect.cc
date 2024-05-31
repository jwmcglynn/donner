#include "src/svg/filter/filter_effect.h"

namespace donner::svg {

bool FilterEffect::operator==(const FilterEffect& other) const {
  return value == other.value;
}

std::ostream& operator<<(std::ostream& os, const FilterEffect& paint) {
  os << "FilterEffect(";
  if (paint.is<FilterEffect::None>()) {
    os << "none";
  } else if (paint.is<FilterEffect::Blur>()) {
    os << "blur(" << paint.get<FilterEffect::Blur>().stdDeviation << ")";
  } else {
    const FilterEffect::ElementReference& ref = paint.get<FilterEffect::ElementReference>();
    os << "url(" << ref.reference.href << ")";
  }
  os << ")";
  return os;
}

}  // namespace donner::svg
