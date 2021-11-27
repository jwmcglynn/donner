#pragma once

#include <compare>
#include <cstdint>
#include <ostream>

namespace donner {
namespace css {

class Specificity {
public:
  constexpr Specificity() = default;
  Specificity(const Specificity& other) = default;
  Specificity(Specificity&& other) = default;

  Specificity& operator=(const Specificity& other) = default;
  Specificity& operator=(Specificity&& other) = default;

  friend std::ostream& operator<<(std::ostream& os, const Specificity& obj) {
    os << "Specificity(";
    if (obj.special_ == SpecialType::Important) {
      os << "!important";
    } else if (obj.special_ == SpecialType::StyleAttribute) {
      os << "style (second highest)";
    } else {
      os << obj.a_ << ", " << obj.b_ << ", " << obj.c_;
    }
    return os << ")";
  }

  static constexpr Specificity FromABC(uint32_t a, uint32_t b, uint32_t c) {
    return Specificity(a, b, c);
  }
  static constexpr Specificity Important() { return Specificity(SpecialType::Important); }
  static constexpr Specificity StyleAttribute() { return Specificity(SpecialType::StyleAttribute); }

  auto operator<=>(const Specificity& other) const {
    if (special_ != other.special_) {
      return special_ <=> other.special_;
    } else if (a_ != other.a_) {
      return a_ <=> other.a_;
    } else if (b_ != other.b_) {
      return b_ <=> other.b_;
    } else {
      return c_ <=> other.c_;
    }
  }

  // For gtest.
  bool operator==(const Specificity& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
  }

private:
  // The order is important here, since operator<=> considers later enum values to be greater.
  enum class SpecialType { None, StyleAttribute, Important };

  constexpr Specificity(uint32_t a, uint32_t b, uint32_t c) : a_(a), b_(b), c_(c) {}
  constexpr Specificity(SpecialType special) : special_(special) {}

  uint32_t a_ = 0;
  uint32_t b_ = 0;
  uint32_t c_ = 0;
  SpecialType special_ = SpecialType::None;
};

}  // namespace css
}  // namespace donner
